# ML pipeline guide *(work in progress)*

> **Status:** The firmware handles data collection only - CSV output, training procedures, active injection. The PC-side training and on-device inference described here are the intended research direction, not finished firmware features. The code snippets are a starting point, not a drop-in integration.

This guide explains how to go from labelled CSV files collected by the firmware to a quantized model running inference on the ESP32-S3 Cardputer.

## why CSI for ML

Each CSI frame is 56 floating-point numbers - the amplitude of each OFDM subcarrier after the radio channel has shaped it. The channel shape encodes multipath: reflections from walls, furniture, and people. When something moves or a person occupies a position, the interference pattern between reflections shifts, and the 56-number fingerprint shifts with it.

This makes CSI useful for:
- **Presence detection** - is someone in the room?
- **Zone classification** - which of N marked positions is a person at?
- **Motion classification** - approaching, receding, still
- **Coarse gesture recognition** - with a well-placed AP and careful data collection

It is not useful for:
- Precise localisation - you need multiple APs and antenna arrays for that
- Identifying who is present
- Working across different rooms or AP placements without retraining

The fingerprint is environmental. A model trained in one room will not work in another without re-collecting data there.

## on-device inference: ESP-DL vs TFLite Micro

The ESP32-S3 Xtensa LX7 cores include a **PIE (Processor Instruction Extension)** - 128-bit SIMD integer instructions. ESP-DL (Espressif's own inference library) is written to exploit these, giving roughly 3-5x faster throughput on convolutions and matrix multiplications compared to generic TFLite Micro, which does scalar int8 arithmetic.

| | ESP-DL | TFLite Micro |
|---|---|---|
| Uses ESP32-S3 PIE SIMD | yes | no |
| Model format | .espdl (via ONNX) | .tflite |
| Supported ops | Conv, Dense, Pool, common activations | larger op set |
| Community docs | thin (Espressif GitHub only) | extensive |
| Recommended for this project | **yes** | fallback |

For a small 1D CNN running at 10 fps, either will work within the compute budget, but ESP-DL is the right long-term choice on ESP32-S3.

## full pipeline overview

```
[Cardputer] collect labelled CSVs via Training mode
    |
[PC] load + inspect data
    |
[PC] feature engineering
    |
[PC] train model (scikit-learn / Keras)
    |
    +-- path A (recommended): Keras -> ONNX -> ESP-DL .espdl -> C array
    +-- path B (fallback):    Keras -> TFLite -> int8 quant  -> C array
    |
[Cardputer] on-device inference at ~10 fps  (planned, not yet in firmware)
```

## 1. load and inspect the data

```python
import pandas as pd
import numpy as np

df = pd.read_csv('session01_indoor.csv')
print(df['label'].value_counts())
print(df.shape)   # expect ~600 rows per 60 s at 10 fps active

amp_cols = [f'a{i:02d}' for i in range(56)]
X_raw = df[amp_cols].values.astype(np.float32)
y     = df['label'].values
```

Load multiple sessions and concatenate before training - one session is never enough.

```python
import glob

frames = []
for path in glob.glob('sessions/*.csv'):
    frames.append(pd.read_csv(path))
df = pd.concat(frames, ignore_index=True)
```

## 2. feature engineering

Raw amplitudes work as a baseline. Adding simple derived features often improves accuracy without adding model complexity.

```python
# per-frame statistics alongside raw amplitudes
var  = X_raw.var(axis=1, keepdims=True)
mean = X_raw.mean(axis=1, keepdims=True)
X_static = np.hstack([X_raw, var, mean])   # shape: (n_frames, 58)
```

For motion classification, frame-to-frame differences carry more signal than the raw values:

```python
diff = np.abs(np.diff(X_raw, axis=0))
diff = np.vstack([diff[:1], diff])          # pad back to same length
X_diff = np.hstack([X_raw, diff])           # shape: (n_frames, 112)
```

For temporal models (CNN, LSTM), build fixed-length windows:

```python
WINDOW = 20   # 20 frames = 2 s at 10 fps
STRIDE = 5

def make_windows(X, y, window, stride):
    Xw, yw = [], []
    for i in range(0, len(X) - window, stride):
        Xw.append(X[i:i+window])
        labels, counts = np.unique(y[i:i+window], return_counts=True)
        yw.append(labels[counts.argmax()])   # majority label in window
    return np.array(Xw), np.array(yw)

Xw, yw = make_windows(X_raw, y, WINDOW, STRIDE)
# Xw shape: (n_windows, 20, 56)
```

## 3. train a model

### option A - Random Forest (good baseline, no GPU needed)

```python
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import LabelEncoder

le = LabelEncoder()
y_enc = le.fit_transform(y)

X_tr, X_te, y_tr, y_te = train_test_split(
    X_static, y_enc, test_size=0.2, stratify=y_enc, random_state=42)

clf = RandomForestClassifier(n_estimators=100, max_depth=12, random_state=42)
clf.fit(X_tr, y_tr)
print(f'accuracy: {clf.score(X_te, y_te):.3f}')
print('classes:', le.classes_)
```

Random Forest is easy to debug, trains in seconds, and often reaches 85-95% accuracy on zone classification with a single AP. It cannot easily be deployed on-device, so use it first to verify that your data has enough signal before investing in a neural network.

### option B - 1D CNN over subcarriers (deployable on-device)

```python
import tensorflow as tf
from sklearn.preprocessing import LabelEncoder

le = LabelEncoder()
yw_enc = le.fit_transform(yw)
n_classes = len(le.classes_)

model = tf.keras.Sequential([
    tf.keras.layers.Input(shape=(WINDOW, 56)),
    tf.keras.layers.Conv1D(32, kernel_size=5, activation='relu', padding='same'),
    tf.keras.layers.MaxPooling1D(2),
    tf.keras.layers.Conv1D(64, kernel_size=3, activation='relu', padding='same'),
    tf.keras.layers.GlobalAveragePooling1D(),
    tf.keras.layers.Dense(64, activation='relu'),
    tf.keras.layers.Dropout(0.3),
    tf.keras.layers.Dense(n_classes, activation='softmax'),
])

model.compile(optimizer='adam',
              loss='sparse_categorical_crossentropy',
              metrics=['accuracy'])
model.fit(Xw, yw_enc, epochs=30, validation_split=0.2, batch_size=32)
```

## 4A. deploy with ESP-DL (recommended for ESP32-S3)

ESP-DL (espressif/esp-dl on GitHub) uses the ESP32-S3 PIE SIMD instructions for integer ops, making it significantly faster than TFLite Micro on convolutions. The model format is .espdl, produced from ONNX.

**Step 1 - export Keras model to ONNX**

```python
# pip install tf2onnx
import tf2onnx, tensorflow as tf

input_sig = [tf.TensorSpec([1, WINDOW, 56], tf.float32, name='input')]
tf2onnx.convert.from_keras(model, input_signature=input_sig,
                            output_path='csi_model.onnx')
```

**Step 2 - quantize and convert to .espdl**

The exact toolchain lives in the espressif/esp-dl repo - refer to its tools/ directory for the current conversion script, as the API evolves between releases. The general flow:

```sh
# install the esp-dl python tools from the repo
pip install -e path/to/esp-dl/tools

python esp_dl_convert.py \
    --input  csi_model.onnx \
    --output csi_model.espdl \
    --calib-data calib_windows.npy   # representative float32 inputs, shape (N, WINDOW, 56)
```

Generate calibration data from your training set:

```python
import numpy as np
calib = Xw[:400].astype(np.float32)   # 400 windows is enough for calibration
np.save('calib_windows.npy', calib)
```

**Step 3 - embed in firmware**

```sh
xxd -i csi_model.espdl > src/csi_model_data.cc
```

**Step 4 - inference sketch (reference, not yet integrated)**

```cpp
#include "dl_model_base.hpp"
#include "csi_model_data.h"   // kCsiModel[], kCsiModelLen

static dl::Model *s_model = nullptr;

void model_init(void) {
    s_model = new dl::Model((const char *)kCsiModel,
                             fbs::MODEL_LOCATION_IN_FLASH_RODATA);
}

// window: WINDOW * 56 floats, row-major
const char* model_infer(float *window) {
    dl::TensorBase *in = s_model->get_input()[0];
    in->assign(window);   // handles float->int8 quantization using stored calibration

    s_model->run();

    dl::TensorBase *out = s_model->get_output()[0];
    int8_t *logits = (int8_t *)out->data;
    int best = 0;
    for (int i = 1; i < out->shape[1]; i++)
        if (logits[i] > logits[best]) best = i;

    // order must match LabelEncoder.classes_ from training
    static const char *kLabels[] = {
        "absent", "approach", "recede", "still_mid", "still_near"
    };
    return kLabels[best];
}
```

The API surface changes between esp-dl releases - check the repo examples/ for the current idiom before using this sketch.

## 4B. deploy with TFLite Micro (fallback)

If ESP-DL's op set does not cover your model, or you want broader documentation, TFLite Micro works on ESP32-S3 without PIE acceleration.

**Convert from Keras:**

```python
def representative_dataset():
    for i in range(0, len(Xw), 10):
        yield [Xw[i:i+1].astype(np.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type  = tf.int8
converter.inference_output_type = tf.int8

tflite_model = converter.convert()
with open('csi_model.tflite', 'wb') as f:
    f.write(tflite_model)
print(f'model size: {len(tflite_model) / 1024:.1f} KB')
# target: under 100 KB to leave flash headroom for the rest of the firmware
```

**Embed in firmware:**

```sh
xxd -i csi_model.tflite > src/csi_model_data.cc
```

**Inference sketch (reference, not yet integrated):**

```cpp
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "csi_model_data.h"

static uint8_t s_arena[64 * 1024];   // tune to model size
static tflite::MicroMutableOpResolver<4> s_resolver;
static tflite::MicroInterpreter         *s_interp = nullptr;

void model_init(void) {
    s_resolver.AddConv2D();
    s_resolver.AddMaxPool2D();
    s_resolver.AddFullyConnected();
    s_resolver.AddSoftmax();
    auto *m = tflite::GetModel(kCsiModel);
    static tflite::MicroInterpreter interp(m, s_resolver, s_arena, sizeof s_arena);
    s_interp = &interp;
    s_interp->AllocateTensors();
}

// window: WINDOW * 56 floats, row-major
const char* model_infer(float window[][56]) {
    TfLiteTensor *in = s_interp->input(0);
    float   scale = in->params.scale;
    int32_t zp    = in->params.zero_point;
    for (int t = 0; t < WINDOW; t++)
        for (int k = 0; k < 56; k++) {
            int q = (int)roundf(window[t][k] / scale) + zp;
            in->data.int8[t * 56 + k] = (int8_t)std::clamp(q, -128, 127);
        }
    s_interp->Invoke();

    TfLiteTensor *out = s_interp->output(0);
    int best = 0;
    for (int i = 1; i < out->dims->data[1]; i++)
        if (out->data.int8[i] > out->data.int8[best]) best = i;

    // order must match LabelEncoder.classes_ from training
    static const char *kLabels[] = {
        "absent", "approach", "recede", "still_mid", "still_near"
    };
    return kLabels[best];
}
```

Call `model_infer()` on a rolling buffer updated each CSI frame. At 10 fps with a 20-frame window you get a new prediction every 100 ms at stride 1.

## realistic expectations

| Task | APs | Expected accuracy |
|------|-----|-------------------|
| Presence (absent vs present) | 1 | 90-97% |
| Zone classification (4 zones) | 1 | 75-90% |
| Motion direction (approach vs recede) | 1 | 80-92% |
| Fine gesture recognition | 1 | 50-70% (varies greatly) |

These numbers assume the same room, same AP position, same type of person, and a model trained in that specific environment. Accuracy drops significantly when anything changes. Retrain when you move the hardware.

## data collection tips

- **5-10 sessions per procedure per environment minimum** before trusting accuracy numbers.
- **Vary conditions** - different times of day, different people, door open vs closed. A model trained in one rigid setup will not generalise.
- **Use active injection** (P key before starting) for consistent 10 fps. Passive fps is unpredictable and makes temporal features unreliable.
- **Validate with held-out sessions**, not just a train/test split of one session - same-session splits are optimistic.
- **Check ChanOcc first** to confirm your AP is delivering a consistent frame rate on your chosen channel before collecting training data.

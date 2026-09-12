/*
 * Vendored from Zephyr PR #96657, "samples: tflite-micro: add micro_speech
 * application with OpenAMP on i.MX8MP" by Thong Phan, which is still open.
 * Copied rather than referenced because this sample needs one behavioural
 * change that PR does not have -- see the sample README. Delete this copy and
 * depend on the upstream sample once that PR merges.
 */
/*
 * Copyright 2025 The TensorFlow Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MICRO_SPEECH_OPENAMP_MODEL_SETTINGS_H_
#define MICRO_SPEECH_OPENAMP_MODEL_SETTINGS_H_

/* The following values are derived from values used during model training.
 * If you change the way you preprocess the input, update all these constants.
 */
constexpr int kAudioSampleFrequency = 16000;
constexpr int kFeatureSize = 40;
constexpr int kFeatureCount = 49;
constexpr int kFeatureElementCount = (kFeatureSize * kFeatureCount);
constexpr int kFeatureStrideMs = 20;
constexpr int kFeatureDurationMs = 30;

/* Variables for the model's output categories. */
constexpr int kCategoryCount = 4;
constexpr const char *kCategoryLabels[kCategoryCount] = {"silence", "unknown", "yes", "no"};

#endif /* MICRO_SPEECH_OPENAMP_MODEL_SETTINGS_H_ */

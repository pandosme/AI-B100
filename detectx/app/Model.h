#ifndef DETECTX_MODEL_H
#define DETECTX_MODEL_H

#include <stddef.h>

#include "cJSON.h"
#include "imgprovider.h"

cJSON* Model_Setup(const cJSON* detection_settings);
cJSON* Model_Inference(VdoBuffer* image);
cJSON* Model_Apply_NMS(cJSON* candidates);
void Model_Cleanup(void);
void Model_Apply_Detection_Settings(cJSON* settings);
const cJSON* Model_Get_Config(void);
int Model_Label_Is_Valid(const char* label);

#endif
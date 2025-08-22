#include "task.h"
#include "utils.h"

void PendSV_Handler(void) { v_log(LOG_DEBUG, "PENDSV Triggered"); }

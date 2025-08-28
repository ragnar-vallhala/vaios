.syntax unified
.cpu cortex-m4
.thumb
.global PendSV_Handler
.global vPortStartFirstTask
.extern current_task
.extern task_get_next_ready
.extern port_save_task_stack_ptr
.extern port_load_task_stack_ptr
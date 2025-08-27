#include "task.h"
#include "structures.h"
#include "memory.h"
#include "utils.h"
#include <stdint.h>


// Dummy tasks
void task_fn1(void *arg) {}
void task_fn2(void *arg) {}
void task_fn3(void *arg) {}

// Print queue for debugging
void print_queue(TCB *head, const char *name) {
    if (!head) {
        v_log(LOG_INFO, "%s is empty", name);
        return;
    }

    TCB *curr = head;
    v_log(LOG_INFO, "Queue %s contents:", name);
    do {
        v_log(LOG_INFO, "  Task id=%d, priority=%d, state=%d", curr->task_id, curr->priority, curr->state);
        curr = curr->next;
    } while (curr != head);
}

// Test enqueue and dequeue
void test_enqueue_dequeue() {
    v_log(LOG_INFO, "== Test: Enqueue and Dequeue ==");

    TCB *head = NULL;
    TCB *t1 = (TCB *)v_malloc(sizeof(TCB));
    t1->task_id = 1;
    TCB *t2 = (TCB *)v_malloc(sizeof(TCB));
    t2->task_id = 2;
    TCB *t3 = (TCB *)v_malloc(sizeof(TCB));
    t3->task_id = 3;

    task_enqueue(&head, t1);
    task_enqueue(&head, t2);
    task_enqueue(&head, t3);
    print_queue(head, "Ready Queue");

    TCB *d = task_dequeue(&head);
    v_log(LOG_INFO, "Dequeued task id=%d", d->task_id);
    v_free(d);

    print_queue(head, "Ready Queue after dequeue");

    // Cleanup remaining tasks
    while ((d = task_dequeue(&head)) != NULL) {
        v_free(d);
    }
}

// Test task_remove
void test_remove() {
    v_log(LOG_INFO, "== Test: Remove from Queue ==");

    TCB *head = NULL;
    TCB *t1 = (TCB *)v_malloc(sizeof(TCB)); t1->task_id = 1;
    TCB *t2 = (TCB *)v_malloc(sizeof(TCB)); t2->task_id = 2;
    TCB *t3 = (TCB *)v_malloc(sizeof(TCB)); t3->task_id = 3;

    task_enqueue(&head, t1);
    task_enqueue(&head, t2);
    task_enqueue(&head, t3);
    print_queue(head, "Queue before remove");

    task_remove(&head, t2);
    v_log(LOG_INFO, "Removed task id=2");
    print_queue(head, "Queue after remove");

    task_remove(&head, t1);
    v_log(LOG_INFO, "Removed task id=1 (head)");
    print_queue(head, "Queue after removing head");

    task_remove(&head, t3);
    v_log(LOG_INFO, "Removed task id=3 (last)");
    print_queue(head, "Queue after removing last");

    v_free(t1); v_free(t2); v_free(t3);
}

// Test insert_sorted by priority
void test_insert_sorted() {
    v_log(LOG_INFO, "== Test: Insert Sorted ==");

    TCB *head = NULL;
    TCB *t1 = (TCB *)v_malloc(sizeof(TCB)); t1->task_id = 1; t1->priority = 1;
    TCB *t2 = (TCB *)v_malloc(sizeof(TCB)); t2->task_id = 2; t2->priority = 3;
    TCB *t3 = (TCB *)v_malloc(sizeof(TCB)); t3->task_id = 3; t3->priority = 2;

    task_insert_sorted(&head, t1);
    task_insert_sorted(&head, t2);
    task_insert_sorted(&head, t3);
    print_queue(head, "Queue after insert_sorted");

    // Check if highest priority is head
    if (head->task_id != 2) {
        v_log(LOG_ERROR, "Head task should be id=2 (highest priority), found id=%d", head->task_id);
    } else {
        v_log(LOG_INFO, "Head task correct after insert_sorted");
    }

    // Cleanup
    TCB *tmp;
    while ((tmp = task_dequeue(&head)) != NULL) {
        v_free(tmp);
    }
}

// Main test
int main(void) {
    v_init();
    heap_memory_init();

    test_enqueue_dequeue();
    test_remove();
    test_insert_sorted();

    v_log(LOG_INFO, "== Task Queue Tests Completed ==");

    return 0;
}

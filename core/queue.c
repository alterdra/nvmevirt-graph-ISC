#include "queue.h"

void queue_init(struct queue *q) {
    INIT_LIST_HEAD(&q->head);
    mutex_init(&q->lock);
    q->size = 0;
}

void queue_enqueue(struct queue *q, struct PROC_EDGE proc_edge_struct) {
    struct queue_node *new_node = kmalloc(sizeof(struct queue_node), GFP_KERNEL);
    if (!new_node) {
        pr_err("Failed to allocate memory for new node\n");
        return;
    }
    new_node->proc_edge_struct = proc_edge_struct;

    mutex_lock(&q->lock);
    list_add_tail(&new_node->list, &q->head);
    q->size++;
    mutex_unlock(&q->lock);
}

int queue_dequeue(struct queue *q, struct PROC_EDGE* proc_edge_struct) {
    struct queue_node *node;
    if (list_empty(&q->head)) {
        return -1; // Queue is empty
    }

    mutex_lock(&q->lock);
    node = list_first_entry(&q->head, struct queue_node, list);
    list_del(&node->list);
    q->size--;
    mutex_unlock(&q->lock);

    *proc_edge_struct = node->proc_edge_struct;
    kfree(node);
    return 0; // Success
}

void queue_destroy(struct queue *q) {
    struct queue_node *node, *tmp;

    mutex_lock(&q->lock);
    list_for_each_entry_safe(node, tmp, &q->head, list) {
        list_del(&node->list);
        kfree(node);
    }
    q->size = 0;
    mutex_unlock(&q->lock);
}

int get_queue_size(struct queue *q){
    int size = -1;
    mutex_lock(&q->lock);
    size = q->size;
    mutex_unlock(&q->lock);
    return size;
}

int get_queue_front(struct queue *q, struct PROC_EDGE* proc_edge_struct) {
    struct queue_node *node;
    if (list_empty(&q->head)) {
        return -1; // Queue is empty
    }
    mutex_lock(&q->lock);
    node = list_first_entry(&q->head, struct queue_node, list);
    mutex_unlock(&q->lock);
    *proc_edge_struct = node->proc_edge_struct;
    return 0; // Success
}


int get_queue_back(struct queue *q, struct PROC_EDGE* proc_edge_struct) {
    struct queue_node *node;
    if (list_empty(&q->head)) {
        return -1; // Queue is empty
    }
    mutex_lock(&q->lock);
    node = list_last_entry(&q->head, struct queue_node, list);
    mutex_unlock(&q->lock);
    *proc_edge_struct = node->proc_edge_struct;
    return 0; // Success
}

void queue_swap(struct queue *q1, struct queue *q2) {
    struct list_head temp_head;
    int temp_size;

    mutex_lock(&q1->lock);
    mutex_lock(&q2->lock);

    INIT_LIST_HEAD(&temp_head);  

    // Move q1->head to temp_head and q2->head to q1->head properly
    list_replace_init(&q1->head, &temp_head);
    list_replace_init(&q2->head, &q1->head);
    list_replace_init(&temp_head, &q2->head);

    // Swap sizes
    temp_size = q1->size;
    q1->size = q2->size;
    q2->size = temp_size;

    mutex_unlock(&q1->lock);
    mutex_unlock(&q2->lock);
}

// bool queue_exist

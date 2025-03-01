#ifndef QUEUE_H
#define QUEUE_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kernel.h>

#include "proc_edge_struct.h"

// Define the queue node structure
struct queue_node {
    struct PROC_EDGE proc_edge_struct;
    struct list_head list;
};

// Define the queue structure
struct queue {
    struct list_head head;
    struct mutex lock;
    int size;
};

// Function declarations
void queue_init(struct queue *q);
void queue_enqueue(struct queue *q, struct PROC_EDGE proc_edge_struct);
int queue_dequeue(struct queue *q, struct PROC_EDGE* proc_edge_struct);
void queue_destroy(struct queue *q);
int get_queue_size(struct queue *q);
int get_queue_front(struct queue *q, struct PROC_EDGE* proc_edge_struct);
int get_queue_back(struct queue *q, struct PROC_EDGE* proc_edge_struct);
void queue_swap(struct queue *q1, struct queue *q2);

#endif // QUEUE_H

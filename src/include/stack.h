#ifndef STACK_H
#define STACK_H
#include <stdbool.h>

// typedef struct _stack Stack;
// typedef struct _node Node;

typedef struct _node
{
    char *line;
    struct _node *prev;
    struct _node *next;
} Node;

typedef struct _stack
{
    Node *current_line;
    int size;
} Stack;

void init_stack(Stack *stack);
void free_stack(Stack *stack);
char *get_next_line(Stack *stack);
char *get_curr_line(Stack *stack);
char *get_prev_line(Stack *stack);
bool push_line(Stack *stack, char *line);

#endif // STACK_H
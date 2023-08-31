#include "include/stack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Node *create_node(char *line)
{
    Node *temp;
    if ((temp = (Node *)malloc(sizeof(Node))) == NULL)
        return NULL;
    temp->line = calloc(strlen(line), sizeof(char));
    strcpy(temp->line, line);
    temp->next = NULL;
    temp->prev = NULL;
    return temp;
}

void init_stack(Stack *stack)
{
    stack->current_line = NULL;
    stack->size = 0;
}
void free_stack(Stack *stack)
{
    // todo
}
char *get_next_line(Stack *stack)
{
    if (stack == NULL || stack->current_line == NULL || stack->current_line->next == NULL)
        return NULL;
    stack->current_line = stack->current_line->next;
    return stack->current_line->line;
}

char *get_curr_line(Stack *stack)
{
    if (stack == NULL || stack->current_line == NULL)
        return NULL;
    return stack->current_line->line;
}

char *get_prev_line(Stack *stack)
{
    if (stack == NULL || stack->current_line == NULL || stack->current_line->prev == NULL)
        return NULL;
    stack->current_line = stack->current_line->prev;
    return stack->current_line->line;
}

bool push_line(Stack *stack, char *line)
{
    Node *newLine = create_node(line);
    if (newLine == NULL)
        return false;
    if (stack->current_line == NULL)
        stack->current_line = newLine;
    else
    {
        Node *curr = stack->current_line;
        if (curr->next != NULL)
            curr->next->prev = newLine;
        newLine->prev = curr;
        newLine->next = curr->next;
        curr->next = newLine;
        stack->current_line = newLine;
    }

    return true;
}

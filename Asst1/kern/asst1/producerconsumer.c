/* This file will contain your solution. Modify it as you wish. */
/*Edited and Changed by z5157656 Choo Yee Hang */
#include <types.h>
#include <lib.h>
#include <synch.h>
#include "producerconsumer_driver.h"
/* Declare any variables you need here to keep track of and
   synchronise your bounded. A sample declaration of a buffer is shown
   below. It is an array of pointers to items.
   
   You can change this if you choose another implementation. 
   However, you should not have a buffer bigger than BUFFER_SIZE 
*/
struct semaphore * full;
struct semaphore * empty;
struct semaphore * buffer_key;

data_item_t * item_buffer[BUFFER_SIZE]; 


/* consumer_receive() is called by a consumer to request more data. It
   should block on a sync primitive if no data is available in your
   buffer. */

data_item_t * consumer_receive(void)
{
    data_item_t * item = NULL;
    P(full);
    P(buffer_key);
    for(int i = 0; i < BUFFER_SIZE; i++){
        if (item_buffer[i] != NULL ){
            item = item_buffer[i];
            item_buffer[i] = NULL;
            break;
        }
    }
    V(buffer_key);
    V(empty);
    
    return item;
    
}


/* procucer_send() is called by a producer to store data in your
   bounded buffer. */

void producer_send(data_item_t *item)
{
    P(empty);
    P(buffer_key);
    for(int i = 0; i < BUFFER_SIZE; i++){
        if (item_buffer[i] == NULL ){
            item_buffer[i] = item;
            break;
        }
    }
    V(buffer_key);
    V(full);
    
}




/* Perform any initialisation (e.g. of global data) you need
   here. Note: You can panic if any allocation fails during setup */

void producerconsumer_startup(void)
{
    full = sem_create("full", 0);
    empty = sem_create("empty", BUFFER_SIZE);
    buffer_key  = sem_create("buffer_key", 1);
    /* initalise array here. */
    for(int i = 0; i < BUFFER_SIZE ; i++){
        item_buffer[i] = NULL;
    }

}


/* Perform any clean-up you need here */
void producerconsumer_shutdown(void)
{
    sem_destroy(full);
    sem_destroy(empty);
    sem_destroy(buffer_key);
}



#include "opt-synchprobs.h"
#include <types.h>  /* required by lib.h */
#include <lib.h>    /* for kprintf */
#include <synch.h>  /* for P(), V(), sem_* */
#include <thread.h> /* for thread_fork() */
#include <test.h>

#include "dining_driver.h"

/*
 * Declare any data structures you might need to synchronise 
 * your forks here.
 * state: 
 * 0 = Thinking - nothing
 * 1 = Hungry - waiting state
 * 2 = Eating - running state
 */

struct semaphore * key;
int state[NUM_PHILOSOPHERS];
struct semaphore sema_queue[NUM_PHILOSOPHERS];

/* formula to get the forks num */

static int left(unsigned long phil_num)
{
     
    return (phil_num - 1 + NUM_PHILOSOPHERS) % NUM_PHILOSOPHERS;
}


static int right(unsigned long phil_num)
{
    return (phil_num + 1) % NUM_PHILOSOPHERS;
}


static void test(unsigned long phil_num)
{
    if(state[left(phil_num)] != 2 && state[right(phil_num)] != 2 && state[phil_num] == 1 )
    {
        state[phil_num] = 2;
        V(&sema_queue[phil_num]); //signal those mad dogs to eat   
    }
}


/*
 * Take forks ensures mutually exclusive access to two forks
 * associated with the philosopher.
 * 
 * The left fork number = phil_num
 * The right fork number = (phil_num + 1) % NUM_PHILOSPHERS
 */

void take_forks(unsigned long phil_num)
{
    //get key
    P(key);
    state[phil_num] = 1;
    test(phil_num);
    V(key);
    // in case of unable to eat
    P(&sema_queue[phil_num]);
}


/*
 * Put forks releases the mutually exclusive access to the
 * philosophers forks.
 */

void put_forks(unsigned long phil_num)
{
    // test neighbour whether they want to eat
    P(key);
    state[phil_num] = 0;
    test(left(phil_num));
    test(right(phil_num));
    V(key);
}


/* 
 * Create gets called before the philosopher threads get started.
 * Insert any initialisation code you require here.
 */

void create_forks()
{
    key = sem_create("mutex_key", 1);
    for(int i = 0; i < NUM_PHILOSOPHERS; i++)
    {
        sema_queue[i] = *sem_create("philo", 0);
    }
}


/*
 * Destroy gets called when the system is shutting down.
 * You should clean up whatever you allocated in create_forks()
 */

void destroy_forks()
{
    sem_destroy(key);
}

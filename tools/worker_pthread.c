/* worker_pthread.c : fil graphique pour les tests sur PC (meme protocole que main_ps3.c :
 * drapeaux + barrieres memoire, le fil secondaire dort par petites periodes). */
#include "../source/n64.h"
#include <pthread.h>
#include <unistd.h>

typedef struct {
    N64Worker w;
    pthread_t th;
    void (*fn)(void *);
    void *arg;
    volatile int go;
} PcWorker;

static void *loop(void *p)
{
    PcWorker *k = (PcWorker *)p;
    for (;;) {
        while (!k->go) usleep(50);
        __sync_synchronize();
        k->fn(k->arg);
        __sync_synchronize();
        k->go = 0;
        k->w.done = 1;
    }
    return NULL;
}

static void start(N64Worker *w, void (*fn)(void *), void *arg)
{
    PcWorker *k = (PcWorker *)w;
    k->fn = fn; k->arg = arg;
    w->done = 0;
    __sync_synchronize();
    k->go = 1;
}

static void wait_(N64Worker *w)
{
    while (!w->done) usleep(20);
    __sync_synchronize();
}

N64Worker *pc_worker_create(void)
{
    static PcWorker k;
    k.w.start = start; k.w.wait = wait_; k.w.done = 1;
    if (pthread_create(&k.th, NULL, loop, &k)) return NULL;
    return &k.w;
}

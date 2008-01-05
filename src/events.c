/*         ______   ___    ___ 
 *        /\  _  \ /\_ \  /\_ \ 
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___ 
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      Event queues.
 *
 *      By Peter Wang.
 *
 *      See readme.txt for copyright information.
 */

/* Title: Event queues
 *
 * An event queue buffers events generated by event sources that were
 * registered with the queue.
 */


#include <string.h>

#include "allegro5/allegro5.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_dtor.h"
#include "allegro5/internal/aintern_events.h"



#define MAX_QUEUE_SIZE  512  /* power of 2 for max use of _AL_VECTOR */


struct ALLEGRO_EVENT_QUEUE
{
   _AL_VECTOR events; /* XXX: better a deque */
   _AL_VECTOR sources;
   _AL_MUTEX mutex;
   _AL_COND cond;
};



/* Function: al_create_event_queue
 *  Create a new, empty event queue, returning a pointer to object if
 *  successful.  Returns NULL on error.
 */
ALLEGRO_EVENT_QUEUE *al_create_event_queue(void)
{
   ALLEGRO_EVENT_QUEUE *queue = _AL_MALLOC(sizeof *queue);

   ASSERT(queue);

   if (queue) {
      _al_vector_init(&queue->events, sizeof(ALLEGRO_EVENT *));
      _al_vector_init(&queue->sources, sizeof(ALLEGRO_EVENT_SOURCE *));
      _AL_MARK_MUTEX_UNINITED(queue->mutex);
      _al_mutex_init(&queue->mutex);
      _al_cond_init(&queue->cond);

      _al_register_destructor(queue, (void (*)(void *)) al_destroy_event_queue);
   }

   return queue;
}



/* Function: al_destroy_event_queue
 *  Destroy the event queue specified.  All event sources currently
 *  registered with the queue will be automatically unregistered before
 *  the queue is destroyed.
 */
void al_destroy_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   _al_unregister_destructor(queue);

   /* Unregister any event sources registered with this queue.  */
   while (_al_vector_is_nonempty(&queue->sources)) {
      ALLEGRO_EVENT_SOURCE **slot = _al_vector_ref_back(&queue->sources);
      al_unregister_event_source(queue, *slot);
   }

   ASSERT(_al_vector_is_empty(&queue->events));
   ASSERT(_al_vector_is_empty(&queue->sources));

   _al_vector_free(&queue->events);
   _al_vector_free(&queue->sources);

   _al_cond_destroy(&queue->cond);
   _al_mutex_destroy(&queue->mutex);

   _AL_FREE(queue);
}



/* Function: al_register_event_source
 *  Register the event source with the event queue specified.  An
 *  event source may be registered with any number of event queues
 *  simultaneously, or none.  Trying to register an event source with
 *  the same event queue more than once does nothing.
 */
void al_register_event_source(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT_SOURCE *source)
{
   ASSERT(queue);
   ASSERT(source);
   {
      ALLEGRO_EVENT_SOURCE **slot;

      if (_al_vector_contains(&queue->sources, &source))
         return;

      _al_event_source_on_registration_to_queue(source, queue);

      slot = _al_vector_alloc_back(&queue->sources);
      *slot = source;
   }
}



/* Function: al_unregister_event_source
 *  Unregister an event source with an event queue.  If the event
 *  source is not actually registered with the event queue, nothing
 *  happens.
 *
 *  If the queue had any events in it which originated from the event
 *  source, they will no longer be in the queue after this call.
 */
void al_unregister_event_source(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT_SOURCE *source)
{
   ASSERT(queue);
   ASSERT(source);

   /* Remove SOURCE from our list.  */
   if (!_al_vector_find_and_delete(&queue->sources, &source))
      return;

   /* Tell the event source that it was unregistered.  */
   _al_event_source_on_unregistration_from_queue(source, queue);

   /* Drop all the events in the queue that belonged to the source.  */
   {
      unsigned int i = 0;
      ALLEGRO_EVENT **slot;
      ALLEGRO_EVENT *event;

      while (i < _al_vector_size(&queue->events)) {
         slot = _al_vector_ref(&queue->events, i);
         event = *slot;

         if (event->any.source == source) {
            _al_release_event(event);
            _al_vector_delete_at(&queue->events, i);
         }
         else
            i++;
      }
   }
}



/* Function: al_event_queue_is_empty
 *  Return true if the event queue specified is currently empty.
 */
bool al_event_queue_is_empty(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   return _al_vector_is_empty(&queue->events);
}



/* get_next_event_if_any: [primary thread]
 *  Helper function.  It returns a pointer to the next event in the
 *  queue, or NULL.  Optionally the event is removed from the queue.
 *  However, the event is _not released_ (which is the caller's
 *  responsibility).  The event queue must be locked before entering
 *  this function.
 */
static ALLEGRO_EVENT *get_next_event_if_any(ALLEGRO_EVENT_QUEUE *queue, bool delete)
{
   if (_al_vector_is_empty(&queue->events))
      return NULL;
   else {
      ALLEGRO_EVENT **slot = _al_vector_ref_front(&queue->events);
      ALLEGRO_EVENT *event = *slot;
      if (delete)
         _al_vector_delete_at(&queue->events, 0); /* inefficient */
      return event;
   }
}



/* get_peek_or_drop_next_event: [primary thread]
 *  Helper function to do the work for al_get_next_event,
 *  al_peek_next_event and al_drop_next_event which are all very
 *  similar.
 */
static bool get_peek_or_drop_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *do_copy, bool do_release)
{
   ALLEGRO_EVENT *next_event;

   _al_mutex_lock(&queue->mutex);
   next_event = get_next_event_if_any(queue, do_release);
   _al_mutex_unlock(&queue->mutex);

   if (!next_event)
      return false;

   if (do_copy)
      _al_copy_event(do_copy, next_event);
   if (do_release)
      _al_release_event(next_event);
   return true;
}



/* Function: al_get_next_event
 *  Take the next event packet out of the event queue specified, and
 *  copy the contents into RET_EVENT, returning true.  The original
 *  event packet will be removed from the queue.  If the event queue is
 *  empty, return false and the contents of RET_EVENT are unspecified.
 */
bool al_get_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ASSERT(queue);
   ASSERT(ret_event);

   return get_peek_or_drop_next_event(queue, ret_event, true);
}



/* Function: al_peek_next_event
 *  Copy the contents of the next event packet in the event queue
 *  specified into RET_EVENT and return true.  The original event
 *  packet will remain at the head of the queue.  If the event queue is
 *  actually empty, this function returns false and the contents of
 *  RET_EVENT are unspecified.
 */
bool al_peek_next_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ASSERT(queue);
   ASSERT(ret_event);

   return get_peek_or_drop_next_event(queue, ret_event, false);
}



/* Function: al_drop_next_event
 *  Drop the next event packet from the queue.  If the queue is empty,
 *  nothing happens.
 */
void al_drop_next_event(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   get_peek_or_drop_next_event(queue, NULL, true);
}



/* Function: al_flush_event_queue
 *  Drops all events, if any, from the queue.
 */
void al_flush_event_queue(ALLEGRO_EVENT_QUEUE *queue)
{
   ASSERT(queue);

   _al_mutex_lock(&queue->mutex);
   {
      while (_al_vector_is_nonempty(&queue->events)) {
         unsigned int i = _al_vector_size(&queue->events) - 1;
         ALLEGRO_EVENT **slot = _al_vector_ref(&queue->events, i);
         ALLEGRO_EVENT *event = *slot;
         _al_vector_delete_at(&queue->events, i);

         /* must release while queue is unlocked */
         _al_mutex_unlock(&queue->mutex);
         _al_release_event(event);
         _al_mutex_lock(&queue->mutex);
      }
   }
   _al_mutex_unlock(&queue->mutex);
}



/* wait_on_queue_forever: [primary thread]
 *  Helper for al_wait_for_event.  The caller must lock the queue
 *  before calling.
 */
static void wait_on_queue_forever(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event)
{
   ALLEGRO_EVENT *next_event = NULL;

   _al_mutex_lock(&queue->mutex);
   {
      while (_al_vector_is_empty(&queue->events))
         _al_cond_wait(&queue->cond, &queue->mutex);

      if (ret_event)
         next_event = get_next_event_if_any(queue, true);
   }
   _al_mutex_unlock(&queue->mutex);

   if (ret_event) {
      /* must release while queue is unlocked */
      ASSERT(next_event);
      _al_copy_event(ret_event, next_event);
      _al_release_event(next_event);
   }
}



/* wait_on_queue_timed: [primary thread]
 *  Helper for al_wait_for_event.  The caller must lock the queue
 *  before calling.
 */
static bool wait_on_queue_timed(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event, long msecs)
{
   unsigned long timeout = al_current_time() + msecs;
   bool timed_out = false;
   ALLEGRO_EVENT *next_event = NULL;

   _al_mutex_lock(&queue->mutex);
   {
      int result = 0;

      /* Is the queue is non-empty?  If not, block on a condition
       * variable, which will be signaled when an event is placed into
       * the queue.
       */
      while (_al_vector_is_empty(&queue->events) && (result != -1))
         result = _al_cond_timedwait(&queue->cond, &queue->mutex, timeout);

      if (result == -1)
         timed_out = true;
      else if (ret_event)
         next_event = get_next_event_if_any(queue, true);
   }
   _al_mutex_unlock(&queue->mutex);

   if (timed_out)
      return false;

   if (ret_event) {
      /* must release with the queue unlocked */
      ASSERT(next_event);
      _al_copy_event(ret_event, next_event);
      _al_release_event(next_event);
   }

   return true;
}



/* Function: al_wait_for_event
 *  Wait until the event queue specified is non-empty.  If RET_EVENT
 *  is not NULL, the first event packet in the queue will be copied
 *  into RET_EVENT and removed from the queue.  If RET_EVENT is NULL
 *  the first event packet is left at the head of the queue.
 *
 *  TIMEOUT_MSECS determines approximately how many milliseconds to
 *  wait.  If it is ALLEGRO_WAIT_FOREVER, the call will wait indefinitely.  If the
 *  call times out, false is returned.  Otherwise true is returned.
 */
bool al_wait_for_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *ret_event, long msecs)
{
   ASSERT(queue);
   ASSERT(msecs == ALLEGRO_WAIT_FOREVER || msecs >= 0);

   if (msecs == ALLEGRO_WAIT_FOREVER) {
      wait_on_queue_forever(queue, ret_event);
      return true;
   }
   else {
      return wait_on_queue_timed(queue, ret_event, msecs);
   }
}



/*----------------------------------------------------------------------*/



/* Internal function: _al_event_queue_push_event
 *  Event sources call this function when they have something to add to
 *  the queue.  If a queue cannot accept the event, the event's
 *  refcount will not be incremented.
 *
 *  If no event queues can accept the event, the event should be
 *  returned to the event source's list of recyclable events.
 */
void _al_event_queue_push_event(ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_EVENT *event)
{
   ASSERT(queue);
   ASSERT(event);

   _al_mutex_lock(&queue->mutex);
   {
      if (_al_vector_size(&queue->events) < MAX_QUEUE_SIZE)
      {
         event->any._refcount++;
         ASSERT(event->any._refcount > 0);

         {
            ALLEGRO_EVENT **slot = _al_vector_alloc_back(&queue->events);
            *slot = event;
         }

         /* Wake up threads that are waiting for an event to be placed in
          * the queue.
          *
          * TODO: Test if multiple threads waiting on the same event
          * queue actually works.
          */
         _al_cond_broadcast(&queue->cond);
      }
   }
   _al_mutex_unlock(&queue->mutex);
}



/* Internal function: _al_copy_event
 *  Copies the contents of the event SRC to DEST.
 */
void _al_copy_event(ALLEGRO_EVENT *dest, const ALLEGRO_EVENT *src)
{
   ASSERT(dest);
   ASSERT(src);

   *dest = *src;

   dest->any._refcount = 0;
   dest->any._next = NULL;
   dest->any._next_free = NULL;
}



/*
 * Local Variables:
 * c-basic-offset: 3
 * indent-tabs-mode: nil
 * End:
 */

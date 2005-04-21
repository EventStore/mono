/*
 * wait.c:  wait for handles to become signalled
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <string.h>
#include <errno.h>

#include <mono/os/gc_wrapper.h>

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/handles-private.h>
#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/mono-mutex.h>
#include <mono/io-layer/misc-private.h>

#undef DEBUG

static gboolean own_if_signalled(gpointer handle)
{
	gboolean ret = FALSE;
	guint32 now = (guint32)(time (NULL) & 0xFFFFFFFF);
	
	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		if (_wapi_handle_shared_trylock_handle (handle, now) == EBUSY) {
			return (FALSE);
		}
	}
	
	if (_wapi_handle_issignalled (handle)) {
		_wapi_handle_ops_own (handle);
		ret = TRUE;
	}

	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		_wapi_handle_shared_unlock_handle (handle, now);
	}

	return(ret);
}

static gboolean own_if_owned(gpointer handle)
{
	gboolean ret = FALSE;
	guint32 now = (guint32)(time (NULL) & 0xFFFFFFFF);
	
	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		if (_wapi_handle_shared_trylock_handle (handle, now) == EBUSY) {
			return (FALSE);
		}
	}
	
	if (_wapi_handle_ops_isowned (handle)) {
		_wapi_handle_ops_own (handle);
		ret = TRUE;
	}

	if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handle))) {
		_wapi_handle_shared_unlock_handle (handle, now);
	}

	return(ret);
}

/**
 * WaitForSingleObjectEx:
 * @handle: an object to wait for
 * @timeout: the maximum time in milliseconds to wait for
 * @alertable: if TRUE, the wait can be interrupted by an APC call
 *
 * This function returns when either @handle is signalled, or @timeout
 * ms elapses.  If @timeout is zero, the object's state is tested and
 * the function returns immediately.  If @timeout is %INFINITE, the
 * function waits forever.
 *
 * Return value: %WAIT_ABANDONED - @handle is a mutex that was not
 * released by the owning thread when it exited.  Ownership of the
 * mutex object is granted to the calling thread and the mutex is set
 * to nonsignalled.  %WAIT_OBJECT_0 - The state of @handle is
 * signalled.  %WAIT_TIMEOUT - The @timeout interval elapsed and
 * @handle's state is still not signalled.  %WAIT_FAILED - an error
 * occurred. %WAIT_IO_COMPLETION - the wait was ended by an APC.
 */
guint32 WaitForSingleObjectEx(gpointer handle, guint32 timeout,
			      gboolean alertable)
{
	guint32 ret, waited;
	struct timespec abstime;
	int thr_ret;
	gboolean apc_pending = FALSE;
	gpointer current_thread = GetCurrentThread ();
	
	if (_wapi_handle_test_capabilities (handle,
					    WAPI_HANDLE_CAP_WAIT) == FALSE) {
#ifdef DEBUG
		g_message ("%s: handle %p can't be waited for", __func__,
			   handle);
#endif

		return(WAIT_FAILED);
	}

	if (_wapi_handle_test_capabilities (handle, WAPI_HANDLE_CAP_SPECIAL_WAIT) == TRUE) {
#ifdef DEBUG
		g_message ("%s: handle %p has special wait", __func__, handle);
#endif

		return (_wapi_handle_ops_special_wait (handle, timeout));
	}
	
	
#ifdef DEBUG
	g_message ("%s: locking handle %p", __func__, handle);
#endif

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      handle);
	thr_ret = _wapi_handle_lock_handle (handle);
	g_assert (thr_ret == 0);

	if (_wapi_handle_test_capabilities (handle,
					    WAPI_HANDLE_CAP_OWN) == TRUE) {
		if (own_if_owned (handle) == TRUE) {
#ifdef DEBUG
			g_message ("%s: handle %p already owned", __func__,
				   handle);
#endif
			ret = WAIT_OBJECT_0;
			goto done;
		}
	}
	
	if (alertable && _wapi_thread_apc_pending (current_thread)) {
		apc_pending = TRUE;
		ret = WAIT_IO_COMPLETION;
		goto done;
	}
	
	if (own_if_signalled (handle) == TRUE) {
#ifdef DEBUG
		g_message ("%s: handle %p already signalled", __func__,
			   handle);
#endif

		ret=WAIT_OBJECT_0;
		goto done;
	}

	/* Have to wait for it */
	if (timeout != INFINITE) {
		_wapi_calc_timeout (&abstime, timeout);
	}
	
	do {
		/* Check before waiting on the condition, just in case
		 */
		if (own_if_signalled (handle)) {
#ifdef DEBUG
			g_message ("%s: handle %p signalled", __func__,
				   handle);
#endif

			ret = WAIT_OBJECT_0;
			goto done;
		}
			
		if (timeout == INFINITE) {
			waited = _wapi_handle_wait_signal_handle (handle);
		} else {
			waited = _wapi_handle_timedwait_signal_handle (handle, &abstime);
		}
	
		if (alertable)
			apc_pending = _wapi_thread_apc_pending (current_thread);

		if(waited==0 && !apc_pending) {
			/* Condition was signalled, so hopefully
			 * handle is signalled now.  (It might not be
			 * if someone else got in before us.)
			 */
			if (own_if_signalled (handle)) {
#ifdef DEBUG
				g_message ("%s: handle %p signalled", __func__,
					   handle);
#endif

				ret=WAIT_OBJECT_0;
				goto done;
			}
		
			/* Better luck next time */
		}
	} while(waited == 0 && !apc_pending);

	/* Timeout or other error */
#ifdef DEBUG
	g_message ("%s: wait on handle %p error: %s", __func__, handle,
		   strerror (waited));
#endif

	ret = WAIT_TIMEOUT;
	
done:

#ifdef DEBUG
	g_message ("%s: unlocking handle %p", __func__, handle);
#endif
	
	thr_ret = _wapi_handle_unlock_handle (handle);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);
	
	if (apc_pending) {
		_wapi_thread_dispatch_apc_queue (current_thread);
		ret = WAIT_IO_COMPLETION;
	}
		
	return(ret);
}

guint32 WaitForSingleObject(gpointer handle, guint32 timeout)
{
	return WaitForSingleObjectEx (handle, timeout, FALSE);
}


/**
 * SignalObjectAndWait:
 * @signal_handle: An object to signal
 * @wait: An object to wait for
 * @timeout: The maximum time in milliseconds to wait for
 * @alertable: Specifies whether the function returnes when the system
 * queues an I/O completion routine or an APC for the calling thread.
 *
 * Atomically signals @signal and waits for @wait to become signalled,
 * or @timeout ms elapses.  If @timeout is zero, the object's state is
 * tested and the function returns immediately.  If @timeout is
 * %INFINITE, the function waits forever.
 *
 * @signal can be a semaphore, mutex or event object.
 *
 * If @alertable is %TRUE and the system queues an I/O completion
 * routine or an APC for the calling thread, the function returns and
 * the thread calls the completion routine or APC function.  If
 * %FALSE, the function does not return, and the thread does not call
 * the completion routine or APC function.  A completion routine is
 * queued when the ReadFileEx() or WriteFileEx() function in which it
 * was specified has completed.  The calling thread is the thread that
 * initiated the read or write operation.  An APC is queued when
 * QueueUserAPC() is called.  Currently completion routines and APC
 * functions are not supported.
 *
 * Return value: %WAIT_ABANDONED - @wait is a mutex that was not
 * released by the owning thread when it exited.  Ownershop of the
 * mutex object is granted to the calling thread and the mutex is set
 * to nonsignalled.  %WAIT_IO_COMPLETION - the wait was ended by one
 * or more user-mode asynchronous procedure calls queued to the
 * thread.  %WAIT_OBJECT_0 - The state of @wait is signalled.
 * %WAIT_TIMEOUT - The @timeout interval elapsed and @wait's state is
 * still not signalled.  %WAIT_FAILED - an error occurred.
 */
guint32 SignalObjectAndWait(gpointer signal_handle, gpointer wait,
			    guint32 timeout, gboolean alertable)
{
	guint32 ret, waited;
	struct timespec abstime;
	int thr_ret;
	gboolean apc_pending = FALSE;
	gpointer current_thread = GetCurrentThread ();
	
	if (_wapi_handle_test_capabilities (signal_handle,
					    WAPI_HANDLE_CAP_SIGNAL)==FALSE) {
		return(WAIT_FAILED);
	}
	
	if (_wapi_handle_test_capabilities (wait,
					    WAPI_HANDLE_CAP_WAIT)==FALSE) {
		return(WAIT_FAILED);
	}

	if (_wapi_handle_test_capabilities (wait, WAPI_HANDLE_CAP_SPECIAL_WAIT) == TRUE) {
		g_warning ("%s: handle %p has special wait, implement me!!",
			   __func__, wait);

		return (WAIT_FAILED);
	}

#ifdef DEBUG
	g_message ("%s: locking handle %p", __func__, wait);
#endif

	pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_handle,
			      wait);
	thr_ret = _wapi_handle_lock_handle (wait);
	g_assert (thr_ret == 0);

	_wapi_handle_ops_signal (signal_handle);

	if (_wapi_handle_test_capabilities (wait, WAPI_HANDLE_CAP_OWN)==TRUE) {
		if (own_if_owned (wait)) {
#ifdef DEBUG
			g_message ("%s: handle %p already owned", __func__,
				   wait);
#endif
			ret = WAIT_OBJECT_0;
			goto done;
		}
	}
	
	if (alertable && _wapi_thread_apc_pending (current_thread)) {
		apc_pending = TRUE;
		ret = WAIT_IO_COMPLETION;
		goto done;
	}
	
	if (own_if_signalled (wait)) {
#ifdef DEBUG
		g_message ("%s: handle %p already signalled", __func__, wait);
#endif

		ret = WAIT_OBJECT_0;
		goto done;
	}

	/* Have to wait for it */
	if (timeout != INFINITE) {
		_wapi_calc_timeout (&abstime, timeout);
	}
	
	do {
		/* Check before waiting on the condition, just in case
		 */
		if (own_if_signalled (wait)) {
#ifdef DEBUG
			g_message ("%s: handle %p signalled", __func__, wait);
#endif

			ret = WAIT_OBJECT_0;
			goto done;
		}
		
		if (timeout == INFINITE) {
			waited = _wapi_handle_wait_signal_handle (wait);
		} else {
			waited = _wapi_handle_timedwait_signal_handle (wait, &abstime);
		}

		if (alertable) {
			apc_pending = _wapi_thread_apc_pending (current_thread);
		}

		if (waited==0 && !apc_pending) {
			/* Condition was signalled, so hopefully
			 * handle is signalled now.  (It might not be
			 * if someone else got in before us.)
			 */
			if (own_if_signalled (wait)) {
#ifdef DEBUG
				g_message ("%s: handle %p signalled", __func__,
					   wait);
#endif

				ret = WAIT_OBJECT_0;
				goto done;
			}
		
			/* Better luck next time */
		}
	} while(waited == 0 && !apc_pending);

	/* Timeout or other error */
#ifdef DEBUG
	g_message ("%s: wait on handle %p error: %s", __func__, wait,
		   strerror (ret));
#endif

	ret = WAIT_TIMEOUT;
	
done:

#ifdef DEBUG
	g_message ("%s: unlocking handle %p", __func__, wait);
#endif

	thr_ret = _wapi_handle_unlock_handle (wait);
	g_assert (thr_ret == 0);
	pthread_cleanup_pop (0);

	if (apc_pending) {
		_wapi_thread_dispatch_apc_queue (current_thread);
		ret = WAIT_IO_COMPLETION;
	}
	
	return(ret);
}

struct handle_cleanup_data
{
	guint32 numobjects;
	gpointer *handles;
	guint32 now;
};

static void handle_cleanup (void *data)
{
	struct handle_cleanup_data *handles = (struct handle_cleanup_data *)data;

	_wapi_handle_unlock_handles (handles->numobjects, handles->handles,
				     handles->now);
}

static gboolean test_and_own (guint32 numobjects, gpointer *handles,
			      gboolean waitall, guint32 *count,
			      guint32 *lowest)
{
	struct handle_cleanup_data cleanup_data;
	gboolean done;
	int i;
	
#ifdef DEBUG
	g_message ("%s: locking handles", __func__);
#endif
	cleanup_data.numobjects = numobjects;
	cleanup_data.handles = handles;
	
	pthread_cleanup_push (handle_cleanup, (void *)&cleanup_data);
	done = _wapi_handle_count_signalled_handles (numobjects, handles,
						     waitall, count, lowest,
						     &cleanup_data.now);
	if (done == TRUE) {
		if (waitall == TRUE) {
			for (i = 0; i < numobjects; i++) {
				own_if_signalled (handles[i]);
			}
		} else {
			own_if_signalled (handles[*lowest]);
		}
	}
	
#ifdef DEBUG
	g_message ("%s: unlocking handles", __func__);
#endif

	/* calls the unlock function */
	pthread_cleanup_pop (1);

	return(done);
}



/**
 * WaitForMultipleObjectsEx:
 * @numobjects: The number of objects in @handles. The maximum allowed
 * is %MAXIMUM_WAIT_OBJECTS.
 * @handles: An array of object handles.  Duplicates are not allowed.
 * @waitall: If %TRUE, this function waits until all of the handles
 * are signalled.  If %FALSE, this function returns when any object is
 * signalled.
 * @timeout: The maximum time in milliseconds to wait for.
 * @alertable: if TRUE, the wait can be interrupted by an APC call
 * 
 * This function returns when either one or more of @handles is
 * signalled, or @timeout ms elapses.  If @timeout is zero, the state
 * of each item of @handles is tested and the function returns
 * immediately.  If @timeout is %INFINITE, the function waits forever.
 *
 * Return value: %WAIT_OBJECT_0 to %WAIT_OBJECT_0 + @numobjects - 1 -
 * if @waitall is %TRUE, indicates that all objects are signalled.  If
 * @waitall is %FALSE, the return value minus %WAIT_OBJECT_0 indicates
 * the first index into @handles of the objects that are signalled.
 * %WAIT_ABANDONED_0 to %WAIT_ABANDONED_0 + @numobjects - 1 - if
 * @waitall is %TRUE, indicates that all objects are signalled, and at
 * least one object is an abandoned mutex object (See
 * WaitForSingleObject() for a description of abandoned mutexes.)  If
 * @waitall is %FALSE, the return value minus %WAIT_ABANDONED_0
 * indicates the first index into @handles of an abandoned mutex.
 * %WAIT_TIMEOUT - The @timeout interval elapsed and no objects in
 * @handles are signalled.  %WAIT_FAILED - an error occurred.
 * %WAIT_IO_COMPLETION - the wait was ended by an APC.
 */
guint32 WaitForMultipleObjectsEx(guint32 numobjects, gpointer *handles,
				 gboolean waitall, guint32 timeout,
				 gboolean alertable)
{
	GHashTable *dups;
	gboolean duplicate = FALSE, bogustype = FALSE, done;
	gboolean shared_wait = FALSE;
	guint32 count, lowest;
	struct timespec abstime;
	guint i;
	guint32 ret;
	int thr_ret;
	gpointer current_thread = GetCurrentThread ();
	
	if (numobjects > MAXIMUM_WAIT_OBJECTS) {
#ifdef DEBUG
		g_message ("%s: Too many handles: %d", __func__, numobjects);
#endif

		return(WAIT_FAILED);
	}
	
	if (numobjects == 1) {
		return WaitForSingleObjectEx (handles [0], timeout, alertable);
	}

	/* Check for duplicates */
	dups = g_hash_table_new (g_direct_hash, g_direct_equal);
	for (i = 0; i < numobjects; i++) {
		gpointer exists = g_hash_table_lookup (dups, handles[i]);
		if (exists != NULL) {
#ifdef DEBUG
			g_message ("%s: Handle %p duplicated", __func__,
				   handles[i]);
#endif

			duplicate = TRUE;
			break;
		}

		if (_wapi_handle_test_capabilities (handles[i], WAPI_HANDLE_CAP_WAIT) == FALSE) {
#ifdef DEBUG
			g_message ("%s: Handle %p can't be waited for",
				   __func__, handles[i]);
#endif

			bogustype = TRUE;
		}

		if (_wapi_handle_test_capabilities (handles[i], WAPI_HANDLE_CAP_SPECIAL_WAIT) == TRUE) {
			g_warning ("%s: handle %p has special wait, implement me!!",
				   __func__, handles[i]);
			
			bogustype = TRUE;
		}

		if (_WAPI_SHARED_HANDLE (_wapi_handle_type (handles[i]))) {
			shared_wait = TRUE;
		}

		g_hash_table_insert (dups, handles[i], handles[i]);
	}
	g_hash_table_destroy (dups);

	if (duplicate == TRUE) {
#ifdef DEBUG
		g_message ("%s: Returning due to duplicates", __func__);
#endif

		return(WAIT_FAILED);
	}

	if (bogustype == TRUE) {
#ifdef DEBUG
		g_message ("%s: Returning due to bogus type", __func__);
#endif

		return(WAIT_FAILED);
	}

	done = test_and_own (numobjects, handles, waitall, &count, &lowest);
	if (done == TRUE) {
		return(WAIT_OBJECT_0+lowest);
	}
	
	/* Have to wait for some or all handles to become signalled
	 */

	if(timeout!=INFINITE) {
		_wapi_calc_timeout (&abstime, timeout);
	}

	if (alertable && _wapi_thread_apc_pending (current_thread)) {
		_wapi_thread_dispatch_apc_queue (current_thread);
		return WAIT_IO_COMPLETION;
	}
	
	while(1) {
		/* Check before waiting on the condition, just in case
		 */
		done = test_and_own (numobjects, handles, waitall,
				     &count, &lowest);
		if (done == TRUE) {
			return(WAIT_OBJECT_0 + lowest);
		}
		
#ifdef DEBUG
		g_message ("%s: locking signal mutex", __func__);
#endif

		pthread_cleanup_push ((void(*)(void *))_wapi_handle_unlock_signal_mutex, NULL);
		thr_ret = _wapi_handle_lock_signal_mutex ();
		g_assert (thr_ret == 0);
		
		if (shared_wait == TRUE) {
			if (timeout == INFINITE) {
				ret = _wapi_handle_wait_signal_poll_share ();
			} else {
				ret = _wapi_handle_timedwait_signal_poll_share (&abstime);
			}
		} else {
			if (timeout == INFINITE) {
				ret = _wapi_handle_wait_signal ();
			} else {
				ret = _wapi_handle_timedwait_signal (&abstime);
			}
		}

#ifdef DEBUG
		g_message ("%s: unlocking signal mutex", __func__);
#endif

		thr_ret = _wapi_handle_unlock_signal_mutex (NULL);
		g_assert (thr_ret == 0);
		pthread_cleanup_pop (0);
		
		if (alertable && _wapi_thread_apc_pending (current_thread)) {
			_wapi_thread_dispatch_apc_queue (current_thread);
			return WAIT_IO_COMPLETION;
		}
	
		if(ret==0) {
			/* Something was signalled ... */
			done = test_and_own (numobjects, handles, waitall,
					     &count, &lowest);
			if(done==TRUE) {
				return(WAIT_OBJECT_0+lowest);
			}
		} else {
			/* Timeout or other error */
#ifdef DEBUG
			g_message ("%s: wait returned error: %s", __func__,
				   strerror (ret));
#endif

			if(ret==ETIMEDOUT) {
				return(WAIT_TIMEOUT);
			} else {
				return(WAIT_FAILED);
			}
		}
	}
}

guint32 WaitForMultipleObjects(guint32 numobjects, gpointer *handles,
			       gboolean waitall, guint32 timeout)
{
	return WaitForMultipleObjectsEx(numobjects, handles, waitall, timeout, FALSE);
}

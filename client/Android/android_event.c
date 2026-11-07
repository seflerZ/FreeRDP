/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Android Event System
 *
 * Copyright 2010-2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>

#include <freerdp/freerdp.h>
#include <freerdp/log.h>

#define TAG CLIENT_TAG("android")

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "android_freerdp.h"
#include "android_cliprdr.h"

BOOL android_push_event(freerdp* inst, ANDROID_EVENT* event)
{
	BOOL rc = FALSE;
	ANDROID_EVENT_QUEUE* queue;
	androidContext* aCtx;

	if (!inst || !inst->context || !event)
		return FALSE;

	aCtx = (androidContext*)inst->context;
	queue = aCtx->event_queue;

	/* 断开后 Java 线程(比如惯性滚动)还可能再 push 一次 */
	if (!queue)
		return FALSE;

	EnterCriticalSection(&queue->lock);

	if (queue->count >= queue->size)
	{
		int new_size;
		void* new_events;
		new_size = queue->size * 2;
		new_events = realloc((void*)queue->events, sizeof(ANDROID_EVENT*) * new_size);

		if (new_events)
		{
			queue->events = new_events;
			queue->size = new_size;
		}
	}

	if (queue->count < queue->size)
	{
		queue->events[(queue->count)++] = event;
		rc = TRUE;
	}

	LeaveCriticalSection(&queue->lock);

	if (rc)
		SetEvent(queue->isSet);

	return rc;
}

static ANDROID_EVENT* android_peek_event(ANDROID_EVENT_QUEUE* queue)
{
	ANDROID_EVENT* event = NULL;

	if (!queue)
		return NULL;

	EnterCriticalSection(&queue->lock);
	if (queue->count > 0)
		event = queue->events[0];
	LeaveCriticalSection(&queue->lock);

	return event;
}

static ANDROID_EVENT* android_pop_event(ANDROID_EVENT_QUEUE* queue)
{
	int i;
	ANDROID_EVENT* event = NULL;

	if (!queue)
		return NULL;

	EnterCriticalSection(&queue->lock);
	if (queue->count > 0)
	{
		event = queue->events[0];
		(queue->count)--;

		for (i = 0; i < queue->count; i++)
		{
			queue->events[i] = queue->events[i + 1];
		}
	}
	LeaveCriticalSection(&queue->lock);

	return event;
}

static BOOL android_process_event(ANDROID_EVENT_QUEUE* queue, freerdp* inst)
{
	ANDROID_EVENT* event;

	if (!inst || !inst->context)
		return TRUE;  // Connection closed, ignore pending events

	rdpContext* context = inst->context;
	androidContext* afc = (androidContext*)context;

	while (android_peek_event(queue))
	{
		event = android_pop_event(queue);

		if (event->type == EVENT_TYPE_KEY)
		{
			ANDROID_EVENT_KEY* key_event = (ANDROID_EVENT_KEY*)event;
			inst->input->KeyboardEvent(inst->input, key_event->flags, key_event->scancode);
			android_event_free((ANDROID_EVENT*)key_event);
		}
		else if (event->type == EVENT_TYPE_KEY_UNICODE)
		{
			ANDROID_EVENT_KEY* key_event = (ANDROID_EVENT_KEY*)event;
			inst->input->UnicodeKeyboardEvent(inst->input, key_event->flags, key_event->scancode);
			android_event_free((ANDROID_EVENT*)key_event);
		}
		else if (event->type == EVENT_TYPE_CURSOR)
		{
			ANDROID_EVENT_CURSOR* cursor_event = (ANDROID_EVENT_CURSOR*)event;
			inst->input->MouseEvent(inst->input, cursor_event->flags, cursor_event->x,
			                        cursor_event->y);
			android_event_free((ANDROID_EVENT*)cursor_event);
		}
		else if (event->type == EVENT_TYPE_CLIPBOARD)
		{
			UINT32 size;
			UINT32 formatId;
			ANDROID_EVENT_CLIPBOARD* clipboard_event = (ANDROID_EVENT_CLIPBOARD*)event;
			formatId = ClipboardRegisterFormat(afc->clipboard, "UTF8_STRING");
			size = clipboard_event->data_length;

			if (size)
				ClipboardSetData(afc->clipboard, formatId, clipboard_event->data, size);
			else
				ClipboardEmpty(afc->clipboard);

			android_cliprdr_send_client_format_list(afc->cliprdr);
			android_event_free((ANDROID_EVENT*)clipboard_event);
		}
		else if (event->type == EVENT_TYPE_TOUCH)
		{
			ANDROID_EVENT_TOUCH* touch_event = (ANDROID_EVENT_TOUCH*)event;
			RdpeiClientContext* rdpei = afc->rdpei;
			
			if (rdpei)
			{
				UINT32 result = CHANNEL_RC_INITIALIZATION_ERROR;
				INT32 actualContactId = touch_event->contactId;
				
				if (touch_event->flags & CONTACT_FLAG_DOWN)
				{
					WLog_DBG(TAG, "Processing queued TouchBegin: (%d, %d), contactId=%d", 
						touch_event->x, touch_event->y, touch_event->contactId);
					result = rdpei->TouchBegin(rdpei, touch_event->contactId, 
						touch_event->x, touch_event->y, &actualContactId);
				}
				else if (touch_event->flags & CONTACT_FLAG_UPDATE)
				{
					WLog_DBG(TAG, "Processing queued TouchUpdate: (%d, %d), contactId=%d", 
						touch_event->x, touch_event->y, touch_event->contactId);
					result = rdpei->TouchUpdate(rdpei, touch_event->contactId, 
						touch_event->x, touch_event->y, &actualContactId);
				}
				else if (touch_event->flags & CONTACT_FLAG_UP)
				{
					WLog_DBG(TAG, "Processing queued TouchEnd: (%d, %d), contactId=%d", 
						touch_event->x, touch_event->y, touch_event->contactId);
					result = rdpei->TouchEnd(rdpei, touch_event->contactId, 
						touch_event->x, touch_event->y, &actualContactId);
				}
				else if (touch_event->flags & CONTACT_FLAG_CANCELED)
				{
					WLog_DBG(TAG, "Processing queued TouchCancel: (%d, %d), contactId=%d", 
						touch_event->x, touch_event->y, touch_event->contactId);
					result = rdpei->TouchEnd(rdpei, touch_event->contactId, 
						touch_event->x, touch_event->y, &actualContactId);
				}
				
				WLog_DBG(TAG, "Touch event processed, result: %lu, actualContactId: %d", 
					result, actualContactId);
			}
			else
			{
				WLog_WARN(TAG, "RDPEI channel not initialized, dropping touch event");
			}
			
			android_event_free((ANDROID_EVENT*)touch_event);
		}
		else if (event->type == EVENT_TYPE_DISCONNECT)
		{
			android_event_free(event);
			return FALSE;
		}
	}

	return TRUE;
}

HANDLE android_get_handle(freerdp* inst)
{
	androidContext* aCtx;

	if (!inst || !inst->context)
		return NULL;

	aCtx = (androidContext*)inst->context;

	if (!aCtx->event_queue || !aCtx->event_queue->isSet)
		return NULL;

	return aCtx->event_queue->isSet;
}

BOOL android_check_handle(freerdp* inst)
{
	androidContext* aCtx;

	if (!inst || !inst->context)
		return FALSE;

	aCtx = (androidContext*)inst->context;

	if (!aCtx->event_queue || !aCtx->event_queue->isSet)
		return FALSE;

	if (WaitForSingleObject(aCtx->event_queue->isSet, 0) == WAIT_OBJECT_0)
	{
		if (!ResetEvent(aCtx->event_queue->isSet))
			return FALSE;

		if (!android_process_event(aCtx->event_queue, inst))
			return FALSE;
	}

	return TRUE;
}

ANDROID_EVENT_KEY* android_event_key_new(int flags, UINT16 scancode)
{
	ANDROID_EVENT_KEY* event;
	event = (ANDROID_EVENT_KEY*)calloc(1, sizeof(ANDROID_EVENT_KEY));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_KEY;
	event->flags = flags;
	event->scancode = scancode;
	return event;
}

static void android_event_key_free(ANDROID_EVENT_KEY* event)
{
	free(event);
}

ANDROID_EVENT_KEY* android_event_unicodekey_new(UINT16 flags, UINT16 key)
{
	ANDROID_EVENT_KEY* event;
	event = (ANDROID_EVENT_KEY*)calloc(1, sizeof(ANDROID_EVENT_KEY));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_KEY_UNICODE;
	event->flags = flags;
	event->scancode = key;
	return event;
}

static void android_event_unicodekey_free(ANDROID_EVENT_KEY* event)
{
	free(event);
}

ANDROID_EVENT_CURSOR* android_event_cursor_new(UINT16 flags, UINT16 x, UINT16 y)
{
	ANDROID_EVENT_CURSOR* event;
	event = (ANDROID_EVENT_CURSOR*)calloc(1, sizeof(ANDROID_EVENT_CURSOR));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_CURSOR;
	event->x = x;
	event->y = y;
	event->flags = flags;
	return event;
}

static void android_event_cursor_free(ANDROID_EVENT_CURSOR* event)
{
	free(event);
}

ANDROID_EVENT* android_event_disconnect_new(void)
{
	ANDROID_EVENT* event;
	event = (ANDROID_EVENT*)calloc(1, sizeof(ANDROID_EVENT));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_DISCONNECT;
	return event;
}

static void android_event_disconnect_free(ANDROID_EVENT* event)
{
	free(event);
}

ANDROID_EVENT_CLIPBOARD* android_event_clipboard_new(const void* data, size_t data_length)
{
	ANDROID_EVENT_CLIPBOARD* event;
	event = (ANDROID_EVENT_CLIPBOARD*)calloc(1, sizeof(ANDROID_EVENT_CLIPBOARD));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_CLIPBOARD;

	if (data)
	{
		event->data = calloc(data_length + 1, sizeof(char));

		if (!event->data)
		{
			free(event);
			return NULL;
		}

		memcpy(event->data, data, data_length);
		event->data_length = data_length + 1;
	}

	return event;
}

static void android_event_clipboard_free(ANDROID_EVENT_CLIPBOARD* event)
{
	if (event)
	{
		free(event->data);
		free(event);
	}
}

ANDROID_EVENT_TOUCH* android_event_touch_new(int x, int y, int flags, int contactId)
{
	ANDROID_EVENT_TOUCH* event;
	event = (ANDROID_EVENT_TOUCH*)calloc(1, sizeof(ANDROID_EVENT_TOUCH));

	if (!event)
		return NULL;

	event->type = EVENT_TYPE_TOUCH;
	event->x = x;
	event->y = y;
	event->flags = flags;
	event->contactId = contactId;
	return event;
}

static void android_event_touch_free(ANDROID_EVENT_TOUCH* event)
{
	free(event);
}

BOOL android_event_queue_init(freerdp* inst)
{
	androidContext* aCtx = (androidContext*)inst->context;
	ANDROID_EVENT_QUEUE* queue;
	queue = (ANDROID_EVENT_QUEUE*)calloc(1, sizeof(ANDROID_EVENT_QUEUE));

	if (!queue)
	{
		WLog_ERR(TAG, "android_event_queue_init: memory allocation failed");
		return FALSE;
	}

	queue->size = 16;
	queue->count = 0;
	InitializeCriticalSection(&queue->lock);
	queue->isSet = CreateEventA(NULL, TRUE, FALSE, NULL);

	if (!queue->isSet)
	{
		DeleteCriticalSection(&queue->lock);
		free(queue);
		return FALSE;
	}

	queue->events = (ANDROID_EVENT**)calloc(queue->size, sizeof(ANDROID_EVENT*));

	if (!queue->events)
	{
		WLog_ERR(TAG, "android_event_queue_init: memory allocation failed");
		CloseHandle(queue->isSet);
		DeleteCriticalSection(&queue->lock);
		free(queue);
		return FALSE;
	}

	aCtx->event_queue = queue;
	return TRUE;
}

void android_event_queue_uninit(freerdp* inst)
{
	androidContext* aCtx;
	ANDROID_EVENT_QUEUE* queue;

	if (!inst || !inst->context)
		return;

	aCtx = (androidContext*)inst->context;
	queue = aCtx->event_queue;

	if (queue)
	{
		/* 先摘掉指针,迟到 push 会直接返回,不去碰正在释放的队列 */
		aCtx->event_queue = NULL;

		EnterCriticalSection(&queue->lock);

		if (queue->isSet)
		{
			CloseHandle(queue->isSet);
			queue->isSet = NULL;
		}

		if (queue->events)
		{
			free(queue->events);
			queue->events = NULL;
			queue->size = 0;
			queue->count = 0;
		}

		LeaveCriticalSection(&queue->lock);
		DeleteCriticalSection(&queue->lock);

		free(queue);
	}
}

void android_event_free(ANDROID_EVENT* event)
{
	if (!event)
		return;

	switch (event->type)
	{
		case EVENT_TYPE_KEY:
			android_event_key_free((ANDROID_EVENT_KEY*)event);
			break;

		case EVENT_TYPE_KEY_UNICODE:
			android_event_unicodekey_free((ANDROID_EVENT_KEY*)event);
			break;

		case EVENT_TYPE_CURSOR:
			android_event_cursor_free((ANDROID_EVENT_CURSOR*)event);
			break;

		case EVENT_TYPE_DISCONNECT:
			android_event_disconnect_free((ANDROID_EVENT*)event);
			break;

		case EVENT_TYPE_CLIPBOARD:
			android_event_clipboard_free((ANDROID_EVENT_CLIPBOARD*)event);
			break;

		case EVENT_TYPE_TOUCH:
			android_event_touch_free((ANDROID_EVENT_TOUCH*)event);
			break;

		default:
			break;
	}
}

/*
  $Id$

  Copyright (C) 1999-2004 IC & S  dbmail@ic-s.nl

  This program is free software; you can redistribute it and/or 
  modify it under the terms of the GNU General Public License 
  as published by the Free Software Foundation; either 
  version 2 of the License, or (at your option) any later 
  version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/**
 * \file dbmail-message.c
 *
 * implements DbmailMessage object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dbmail.h"
#include "dbmail-message.h"
#include "db.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern db_param_t _db_params;
#define DBPFX _db_params.pfx

/* for issuing queries to the backend */
char query[DEF_QUERYSIZE];

/*
 * _register_header
 *
 * register a message header in a ghashtable dictionary
 *
 */
static void _register_header(const char *header, const char *value, gpointer user_data);

/*
 * _retrieve
 *
 * retrieve message for id
 *
 */

static struct DbmailMessage * _retrieve(struct DbmailMessage *self, char *query_template);
static void _map_headers(struct DbmailMessage *self);
static void _set_message(struct DbmailMessage *self, const GString *message);
static void _set_message_from_stream(struct DbmailMessage *self, GMimeStream *stream);


static void _register_header(const char *header, const char *value, gpointer user_data)
{
	g_hash_table_insert((GHashTable *)user_data, (gpointer)header, (gpointer)value);
}
	

static void _map_headers(struct DbmailMessage *self) 
{
	GHashTable *dict = g_hash_table_new((GHashFunc)g_str_hash, (GEqualFunc)g_str_equal);
	assert(self->message);
	g_mime_header_foreach(GMIME_OBJECT(self->message)->headers, _register_header, dict);
	self->headers = dict;
}

static struct DbmailMessage * _retrieve(struct DbmailMessage *self, char *query_template)
{
	
	int row = 0, rows = 0;
	GString *message = g_string_new("");
	
	assert(self->id != 0);
	
	snprintf(query, DEF_QUERYSIZE, query_template, DBPFX, DBPFX, self->id);

	if (db_query(query) == -1) {
		trace(TRACE_ERROR, "%s,%s: sql error", __FILE__, __func__);
		return NULL;
	}

	if (! (rows = db_num_rows())) {
		trace(TRACE_ERROR, "%s,%s: blk error", __FILE__, __func__);
		db_free_result();
		return NULL;	/* msg should have 1 block at least */
	}

	for (row=0; row < rows; row++)
		message = g_string_append(message, db_get_result(row, 0));
	db_free_result();
	
	return dbmail_message_init(self,message);
}

/*
 *
 * retrieve the header messageblk
 *
 * TODO: this call is yet unused in the code, but here for
 * forward compatibility's sake.
 *
 */
static void _fetch_head(struct DbmailMessage *self)
{
	char *query_template = 	"SELECT block.messageblk "
		"FROM %smessageblks block, %smessages msg "
		"WHERE block.physmessage_id = msg.physmessage_id "
		"AND block.is_header = 1"
		"AND msg.message_idnr = '%llu' "
		"ORDER BY block.messageblk_idnr";
	_retrieve(self, query_template);

}

/*
 *
 * retrieve the full message
 *
 */
static void _fetch_full(struct DbmailMessage *self) 
{
	char *query_template = "SELECT block.messageblk "
		"FROM %smessageblks block, %smessages msg "
		"WHERE block.physmessage_id = msg.physmessage_id "
		"AND msg.message_idnr = '%llu' "
		"ORDER BY block.messageblk_idnr";
	_retrieve(self, query_template);
}

struct DbmailMessage * dbmail_message_new(void)
{
	struct DbmailMessage *self = (struct DbmailMessage *)my_malloc(sizeof(struct DbmailMessage));
	
	g_mime_init(0);
	if (! self) {
		trace(TRACE_ERROR, "%s,%s: memory error", __FILE__, __func__);
		return NULL;
	}
	self->id=0;
	self->message=NULL;
	self->headers=NULL;
	self->size=0;
	self->rfcsize=0;
	return self;
}

char * dbmail_message_get_headers_as_string(struct DbmailMessage *self)
{
	char *result;
	GString *headers = g_string_new(g_mime_object_get_headers((GMimeObject *)(self->message)));
	result = headers->str;
	g_string_free(headers,FALSE);
	return result;
}

char * dbmail_message_get_body_as_string(struct DbmailMessage *self)
{
	char *result;
	GString *header = g_string_new(g_mime_object_get_headers((GMimeObject *)(self->message)));
	GString *body = g_string_new(g_mime_object_to_string((GMimeObject *)(self->message)));
	body = g_string_erase(body,0,header->len);
	result = body->str;
	g_string_free(body,FALSE);
	g_string_free(header,TRUE);
	return result;
}

void dbmail_message_destroy(struct DbmailMessage *self)
{
	g_hash_table_destroy(self->headers);
	g_object_unref(self->message);
	self->headers=NULL;
	self->message=NULL;
	self->id=0;
	self->size=0;
	self->rfcsize=0;
	my_free(self);
}

struct DbmailMessage * dbmail_message_retrieve(struct DbmailMessage *self, u64_t id,int filter)
{
	self->id = id;
	
	switch (filter) {
		case DBMAIL_MESSAGE_FILTER_HEAD:
			_fetch_head(self);
			break;
		case DBMAIL_MESSAGE_FILTER_FULL:
			_fetch_full(self);
			break;
	}
	
	if (! self->message) {
		trace(TRACE_ERROR, 
				"%s,%s: retrieval failed for id [%llu]", 
				__FILE__, __func__, id
				);
		return NULL;
	}

	return self;
}

struct DbmailMessage * dbmail_message_init(struct DbmailMessage *self, const GString *message)
{
	_set_message(self,message);
	_map_headers(self);
	self->rfcsize = dbmail_message_get_rfcsize(self);
	return self;
}

static void _set_message(struct DbmailMessage *self, const GString *message)
{
	GMimeStream *stream = g_mime_stream_mem_new_with_buffer(message->str, message->len);
	_set_message_from_stream(self, stream);
	g_object_unref(stream);
}

struct DbmailMessage * dbmail_message_init_with_stream(struct DbmailMessage *self, GMimeStream *stream)
{
	_set_message_from_stream(self,stream);
	_map_headers(self);
	self->rfcsize = dbmail_message_get_rfcsize(self);
	return self;
}

static void _set_message_from_stream(struct DbmailMessage *self, GMimeStream *stream)
{
	/* 
	 * We convert all messages to crlf->lf for internal usage and
	 * db-insertion
	 */
	
	GMimeStream *ostream, *fstream;
	GMimeFilter *filter;
	GMimeParser *parser;

	ostream = g_mime_stream_mem_new();
	fstream = g_mime_stream_filter_new_with_stream(ostream);
	filter = g_mime_filter_crlf_new(GMIME_FILTER_CRLF_DECODE,GMIME_FILTER_CRLF_MODE_CRLF_ONLY);
	
	g_mime_stream_filter_add((GMimeStreamFilter *) fstream, filter);
	g_mime_stream_write_to_stream(stream,fstream);
	g_mime_stream_reset(ostream);
	
	parser = g_mime_parser_new_with_stream(ostream);
	
	self->message = g_mime_parser_construct_message(parser);
	self->size = g_mime_stream_length(ostream);
	
	g_object_unref(filter);
	g_object_unref(fstream);
	g_object_unref(ostream);
	g_object_unref(parser);
}

size_t dbmail_message_get_rfcsize(struct DbmailMessage *self) 
{
	/*
	 * We convert all messages lf->crlf in-memory to determine
	 * the rfcsize
	 */
	

	GMimeStream *ostream, *fstream;
	GMimeFilter *filter;

	if (self->rfcsize)
		return self->rfcsize;
	
	ostream = g_mime_stream_mem_new();
	fstream = g_mime_stream_filter_new_with_stream(ostream);
	filter = g_mime_filter_crlf_new(GMIME_FILTER_CRLF_ENCODE,GMIME_FILTER_CRLF_MODE_CRLF_ONLY);
	
	g_mime_stream_filter_add((GMimeStreamFilter *) fstream, filter);
	g_mime_object_write_to_stream((GMimeObject *)self->message,fstream);
	
	self->rfcsize = g_mime_stream_length(ostream);
	
	g_object_unref(filter);
	g_object_unref(fstream);
	g_object_unref(ostream);

	return self->rfcsize;
}


/*******************************************************************************
 *
 *                              Delta Chat Core
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 *******************************************************************************
 *
 * File:    mrmimefactory.c
 *
 ******************************************************************************/


#include <stdlib.h>
#include <string.h>
#include "mrmailbox.h"
#include "mrmimefactory.h"
#include "mrtools.h"


/*******************************************************************************
 * Set data
 ******************************************************************************/


void mrmimefactory_init(mrmimefactory_t* factory, mrmailbox_t* mailbox)
{
	if( factory == NULL || mailbox == NULL ) {
		return;
	}

	memset(factory, 0, sizeof(mrmimefactory_t));
	factory->m_mailbox = mailbox;
}


void mrmimefactory_empty(mrmimefactory_t* factory)
{
	if( factory == NULL ) {
		return;
	}

	free(factory->m_from_addr);
	factory->m_from_addr = NULL;

	free(factory->m_from_displayname);
	factory->m_from_displayname = NULL;

	if( factory->m_recipients_names ) {
		clist_free_content(factory->m_recipients_names);
		clist_free(factory->m_recipients_names);
		factory->m_recipients_names = NULL;
	}

	if( factory->m_recipients_names ) {
		clist_free_content(factory->m_recipients_addr);
		clist_free(factory->m_recipients_addr);
		factory->m_recipients_addr = NULL;
	}

	mrmsg_unref(factory->m_msg);
	factory->m_msg = NULL;

	mrchat_unref(factory->m_chat);
	factory->m_chat = NULL;

	if( factory->m_out ) {
		mmap_string_free(factory->m_out);
		factory->m_out = NULL;
	}
	factory->m_out_encrypted = 0;
}


int mrmimefactory_load_msg(mrmimefactory_t* factory, uint32_t msg_id)
{
	if( factory == NULL || msg_id <= MR_MSG_ID_LAST_SPECIAL
	 || factory->m_mailbox == NULL
	 || factory->m_msg /*call empty() before */ ) {
		goto cleanup;
	}

	mrmailbox_t* mailbox = factory->m_mailbox;

	factory->m_recipients_names = clist_new();
	factory->m_recipients_addr  = clist_new();
	factory->m_msg              = mrmsg_new();
	factory->m_chat             = mrchat_new(mailbox);

	int success = 0;
	mrsqlite3_lock(mailbox->m_sql);
		if( mrmsg_load_from_db__(factory->m_msg, mailbox, msg_id)
		 && mrchat_load_from_db__(factory->m_chat, factory->m_msg->m_chat_id) )
		{
			sqlite3_stmt* stmt = mrsqlite3_predefine__(mailbox->m_sql, SELECT_na_FROM_chats_contacs_JOIN_contacts_WHERE_cc,
				"SELECT c.authname, c.addr FROM chats_contacts cc LEFT JOIN contacts c ON cc.contact_id=c.id WHERE cc.chat_id=? AND cc.contact_id>?;");
			sqlite3_bind_int(stmt, 1, factory->m_msg->m_chat_id);
			sqlite3_bind_int(stmt, 2, MR_CONTACT_ID_LAST_SPECIAL);
			while( sqlite3_step(stmt) == SQLITE_ROW )
			{
				const char* authname = (const char*)sqlite3_column_text(stmt, 0);
				const char* addr = (const char*)sqlite3_column_text(stmt, 1);
				if( clist_search_string_nocase(factory->m_recipients_addr, addr)==0 )
				{
					clist_append(factory->m_recipients_names, (void*)((authname&&authname[0])? safe_strdup(authname) : NULL));
					clist_append(factory->m_recipients_addr,  (void*)safe_strdup(addr));
				}
			}

			int system_command = mrparam_get_int(factory->m_msg->m_param, 'S', 0);
			if( system_command==MR_SYSTEM_MEMBER_REMOVED_FROM_GROUP /* for added members, the list is just fine */) {
				char* email_to_remove = mrparam_get(factory->m_msg->m_param, 'E', NULL);
				char* self_addr = mrsqlite3_get_config__(mailbox->m_sql, "configured_addr", "");
				if( email_to_remove && strcasecmp(email_to_remove, self_addr)!=0 )
				{
					if( clist_search_string_nocase(factory->m_recipients_addr, email_to_remove)==0 )
					{
						clist_append(factory->m_recipients_names, NULL);
						clist_append(factory->m_recipients_addr,  (void*)email_to_remove);
					}
				}
				free(self_addr);
			}

			factory->m_from_addr        = mrsqlite3_get_config__(mailbox->m_sql, "configured_addr", NULL);
			factory->m_from_displayname = mrsqlite3_get_config__(mailbox->m_sql, "displayname", NULL);

			factory->m_req_readreceipt = 0;
			if( mrsqlite3_get_config_int__(mailbox->m_sql, "readreceipts", MR_READRECEIPTS_DEFAULT) ) {
				if( clist_count(factory->m_recipients_addr)==1 ) { /* for groups, read receipts are a little bit more complicated to receive */
					factory->m_req_readreceipt = 1;
				}
			}

			/* Get a predecessor of the mail to send.
			For simplicity, we use the last message send not by us.
			This is not 100% accurate and may even be a newer message if first sending fails and new messages arrive -
			however, as we currently only use it to identifify answers from different email addresses, this is sufficient.

			Our first idea was to write the predecessor to the `In-Reply-To:` header, however, this results
			in infinite depth thread views eg. in thunderbird.  Maybe we can work around this issue by using only one
			predecessor anchor a day, however, for the moment, we just use the `X-MrPredecessor` header that does not
			disturb other mailers.

			Finally, maybe the Predecessor/In-Reply-To header is not needed for all answers but only to the first ones -
			or after the sender has changes its email address. */
			stmt = mrsqlite3_predefine__(mailbox->m_sql, SELECT_rfc724_FROM_msgs_ORDER_BY_timestamp_LIMIT_1,
				"SELECT rfc724_mid FROM msgs WHERE timestamp=(SELECT max(timestamp) FROM msgs WHERE chat_id=? AND from_id!=?);");
			sqlite3_bind_int  (stmt, 1, factory->m_msg->m_chat_id);
			sqlite3_bind_int  (stmt, 2, MR_CONTACT_ID_SELF);
			if( sqlite3_step(stmt) == SQLITE_ROW ) {
				factory->m_predecessor = strdup_keep_null((const char*)sqlite3_column_text(stmt, 0));
			}

			/* get a References:-header: either the same as the last one or a random one */
			time_t prev_msg_time = 0;
			stmt = mrsqlite3_predefine__(mailbox->m_sql, SELECT_MAX_timestamp_FROM_msgs,
				"SELECT max(timestamp) FROM msgs WHERE chat_id=? AND id!=?");
			sqlite3_bind_int  (stmt, 1, factory->m_msg->m_chat_id);
			sqlite3_bind_int  (stmt, 2, factory->m_msg->m_id);
			if( sqlite3_step(stmt) == SQLITE_ROW ) {
				prev_msg_time = sqlite3_column_int64(stmt, 0);
			}

			#define NEW_THREAD_THRESHOLD 1*60*60
			if( prev_msg_time != 0 && factory->m_msg->m_timestamp - prev_msg_time < NEW_THREAD_THRESHOLD ) {
				factory->m_references = mrparam_get(factory->m_chat->m_param, 'R', NULL);
			}

			if( factory->m_references == NULL ) {
				factory->m_references = mr_create_dummy_references_mid();
				mrparam_set(factory->m_chat->m_param, 'R', factory->m_references);
				mrchat_update_param__(factory->m_chat);
			}

			success = 1;
		}

		if( success ) {
			factory->m_increation = mrmsg_is_increation__(factory->m_msg);
		}
	mrsqlite3_unlock(mailbox->m_sql);

cleanup:
	return success;
}


/*******************************************************************************
 * Render
 ******************************************************************************/


static struct mailmime* build_body_text(char* text)
{
	struct mailmime_fields*    mime_fields;
	struct mailmime*           message_part;
	struct mailmime_content*   content;

	content = mailmime_content_new_with_str("text/plain");
	clist_append(content->ct_parameters, mailmime_param_new_with_data("charset", "utf-8")); /* format=flowed currently does not really affect us, see https://www.ietf.org/rfc/rfc3676.txt */

	mime_fields = mailmime_fields_new_encoding(MAILMIME_MECHANISM_8BIT);

	message_part = mailmime_new_empty(content, mime_fields);
	mailmime_set_body_text(message_part, text, strlen(text));

	return message_part;
}


static struct mailmime* build_body_file(const mrmsg_t* msg)
{
	struct mailmime_fields*  mime_fields;
	struct mailmime*         mime_sub = NULL;
	struct mailmime_content* content;

	char* pathNfilename = mrparam_get(msg->m_param, 'f', NULL);
	char* mimetype = mrparam_get(msg->m_param, 'm', NULL);
	char* suffix = mr_get_filesuffix_lc(pathNfilename);
	char* filename_to_send = NULL;

	if( pathNfilename == NULL ) {
		goto cleanup;
	}

	/* get file name to use for sending (for privacy purposes, we do not transfer the original filenames eg. for images; these names are normally not needed and contain timesamps, running numbers etc.) */
	if( msg->m_type == MR_MSG_VOICE ) {
		struct tm wanted_struct;
		memcpy(&wanted_struct, localtime(&msg->m_timestamp), sizeof(struct tm));
		filename_to_send = mr_mprintf("voice-message_%04i-%02i-%02i_%02i-%02i-%02i.%s",
			(int)wanted_struct.tm_year+1900, (int)wanted_struct.tm_mon+1, (int)wanted_struct.tm_mday,
			(int)wanted_struct.tm_hour, (int)wanted_struct.tm_min, (int)wanted_struct.tm_sec,
			suffix? suffix : "dat");
	}
	else if( msg->m_type == MR_MSG_AUDIO ) {
		char* author = mrparam_get(msg->m_param, 'N', NULL);
		char* title = mrparam_get(msg->m_param, 'n', NULL);
		if( author && author[0] && title && title[0] && suffix ) {
			filename_to_send = mr_mprintf("%s - %s.%s",  author, title, suffix); /* the separator ` - ` is used on the receiver's side to construct the information; we avoid using ID3-scanners for security purposes */
		}
		else {
			filename_to_send = mr_get_filename(pathNfilename);
		}
		free(author);
		free(title);
	}
	else if( msg->m_type == MR_MSG_IMAGE || msg->m_type == MR_MSG_GIF ) {
		filename_to_send = mr_mprintf("image.%s", suffix? suffix : "dat");
	}
	else if( msg->m_type == MR_MSG_VIDEO ) {
		filename_to_send = mr_mprintf("video.%s", suffix? suffix : "dat");
	}
	else {
		filename_to_send = mr_get_filename(pathNfilename);
	}

	/* check mimetype */
	if( mimetype == NULL && suffix != NULL ) {
		if( strcmp(suffix, "png")==0 ) {
			mimetype = safe_strdup("image/png");
		}
		else if( strcmp(suffix, "jpg")==0 || strcmp(suffix, "jpeg")==0 || strcmp(suffix, "jpe")==0 ) {
			mimetype = safe_strdup("image/jpeg");
		}
		else if( strcmp(suffix, "gif")==0 ) {
			mimetype = safe_strdup("image/gif");
		}
		else {
			mimetype = safe_strdup("application/octet-stream");
		}
	}

	if( mimetype == NULL ) {
		goto cleanup;
	}

	/* create mime part */
	mime_fields = mailmime_fields_new_filename(MAILMIME_DISPOSITION_TYPE_ATTACHMENT,
		safe_strdup(filename_to_send), MAILMIME_MECHANISM_BASE64);

	content = mailmime_content_new_with_str(mimetype);

	mime_sub = mailmime_new_empty(content, mime_fields);

	mailmime_set_body_file(mime_sub, safe_strdup(pathNfilename));

cleanup:
	free(pathNfilename);
	free(mimetype);
	free(filename_to_send);
	free(suffix);
	return mime_sub;
}


static char* get_subject(const mrchat_t* chat, const mrmsg_t* msg, const char* afwd_email)
{
	char *ret, *raw_subject = mrmsg_get_summarytext_by_raw(msg->m_type, msg->m_text, msg->m_param, APPROX_SUBJECT_CHARS);
	const char* fwd = afwd_email? "Fwd: " : "";

	if( chat->m_type==MR_CHAT_GROUP )
	{
		ret = mr_mprintf(MR_CHAT_PREFIX " %s: %s%s", chat->m_name, fwd, raw_subject);
	}
	else
	{
		ret = mr_mprintf(MR_CHAT_PREFIX " %s%s", fwd, raw_subject);
	}

	free(raw_subject);
	return ret;
}


int mrmimefactory_render(mrmimefactory_t* factory, int encrypt_to_self)
{
	if( factory == NULL
	 || factory->m_out/*call empty() before*/ ) {
		return 0;
	}

	mrmsg_t*                     msg = factory->m_msg;
	mrchat_t*                    chat = factory->m_chat;
	struct mailimf_fields*       imf_fields;
	struct mailmime*             message = NULL;
	char*                        message_text = NULL, *subject_str = NULL;
	char*                        afwd_email = mrparam_get(msg->m_param, 'a', NULL);
	int                          col = 0;
	int                          success = 0;
	int                          parts = 0;
	mrmailbox_e2ee_helper_t      e2ee_helper;

	memset(&e2ee_helper, 0, sizeof(mrmailbox_e2ee_helper_t));

	/* create empty mail */
	{
		struct mailimf_mailbox_list* from = mailimf_mailbox_list_new_empty();
		mailimf_mailbox_list_add(from, mailimf_mailbox_new(factory->m_from_displayname? mr_encode_header_string(factory->m_from_displayname) : NULL, safe_strdup(factory->m_from_addr)));

		struct mailimf_address_list* to = NULL;
		if( factory->m_recipients_names && factory->m_recipients_addr && clist_count(factory->m_recipients_addr)>0 ) {
			clistiter *iter1, *iter2;
			to = mailimf_address_list_new_empty();
			for( iter1=clist_begin(factory->m_recipients_names),iter2=clist_begin(factory->m_recipients_addr);  iter1!=NULL&&iter2!=NULL;  iter1=clist_next(iter1),iter2=clist_next(iter2)) {
				const char* name = clist_content(iter1);
				const char* addr = clist_content(iter2);
				mailimf_address_list_add(to, mailimf_address_new(MAILIMF_ADDRESS_MAILBOX, mailimf_mailbox_new(name? mr_encode_header_string(name) : NULL, safe_strdup(addr)), NULL));
			}
		}

		clist* references_list = clist_new();
		clist_append(references_list,  (void*)safe_strdup(factory->m_references));
		imf_fields = mailimf_fields_new_with_data_all(mailimf_get_date(msg->m_timestamp), from,
			NULL /* sender */, NULL /* reply-to */,
			to, NULL /* cc */, NULL /* bcc */, safe_strdup(msg->m_rfc724_mid), NULL /* in-reply-to */,
			references_list /* references */,
			NULL /* subject set later */);

		/* add additional basic parameters */
		mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-Mailer"), mr_mprintf("MrMsg %i.%i.%i", MR_VERSION_MAJOR, MR_VERSION_MINOR, MR_VERSION_REVISION))); /* only informational, for debugging, may be removed in the release */
		mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrMsg"), strdup("1.0"))); /* mark message as being sent by a messenger */
		if( factory->m_predecessor ) {
			mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrPredecessor"), strdup(factory->m_predecessor)));
		}

		if( factory->m_req_readreceipt ) {
			mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("Disposition-Notification-To"), strdup(factory->m_from_addr)));
		}

		/* add additional group paramters */
		if( chat->m_type==MR_CHAT_GROUP )
		{
			mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrGrpId"), safe_strdup(chat->m_grpid)));
			mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrGrpName"), mr_encode_header_string(chat->m_name)));

			int system_command = mrparam_get_int(msg->m_param, 'S', 0);
			if( system_command == MR_SYSTEM_MEMBER_REMOVED_FROM_GROUP ) {
				char* email_to_remove = mrparam_get(msg->m_param, 'E', NULL);
				if( email_to_remove ) {
					mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrRemoveFromGrp"), email_to_remove));
				}
			}
			else if( system_command == MR_SYSTEM_MEMBER_ADDED_TO_GROUP ) {
				char* email_to_add = mrparam_get(msg->m_param, 'E', NULL);
				if( email_to_add ) {
					mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrAddToGrp"), email_to_add));
				}
			}
			else if( system_command == MR_SYSTEM_GROUPNAME_CHANGED ) {
				mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrGrpNameChanged"), strdup("1")));
			}
		}

		/* add additional media paramters */
		if( msg->m_type == MR_MSG_VOICE || msg->m_type == MR_MSG_AUDIO || msg->m_type == MR_MSG_VIDEO )
		{
			if( msg->m_type == MR_MSG_VOICE ) {
				mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrVoiceMessage"), strdup("1")));
			}

			int duration_ms = mrparam_get_int(msg->m_param, 'd', 0);
			if( duration_ms > 0 ) {
				mailimf_fields_add(imf_fields, mailimf_field_new_custom(strdup("X-MrDurationMs"), mr_mprintf("%i", (int)duration_ms)));
			}
		}

	}

	message = mailmime_new_message_data(NULL);
	mailmime_set_imf_fields(message, imf_fields);

	/* add text part - we even add empty text and force a MIME-message as:
	- some Apps have problems with Non-text in the main part (eg. "Mail" from stock Android)
	- we can add "forward hints" this way
	- it looks better */
	{
		char* fwdhint = NULL;
		if( afwd_email ) {
			char* afwd_name = mrparam_get(msg->m_param, 'A', NULL);
				char* nameNAddr = mr_get_headerlike_name(afwd_email, afwd_name);
					fwdhint = mr_mprintf("---------- Forwarded message ----------\nFrom: %s\n\n", nameNAddr); /* no not chage this! expected this way in the simplifier to detect forwarding! */
				free(nameNAddr);
			free(afwd_name);
		}

		int write_m_text = 0;
		if( msg->m_type==MR_MSG_TEXT && msg->m_text && msg->m_text[0] ) { /* m_text may also contain data otherwise, eg. the filename of attachments */
			write_m_text = 1;
		}

		char* footer = mrstock_str(MR_STR_STATUSLINE);
		message_text = mr_mprintf("%s%s%s%s%s",
			fwdhint? fwdhint : "",
			write_m_text? msg->m_text : "",
			(write_m_text&&footer&&footer[0])? "\n\n" : "",
			(footer&&footer[0])? "-- \n"  : "",
			(footer&&footer[0])? footer       : "");
		free(footer);
		struct mailmime* text_part = build_body_text(message_text);
		mailmime_smart_add_part(message, text_part);
		parts++;

		free(fwdhint);
	}

	/* add attachment part */
	if( MR_MSG_NEEDS_ATTACHMENT(msg->m_type) ) {
		struct mailmime* file_part = build_body_file(msg);
		if( file_part ) {
			mailmime_smart_add_part(message, file_part);
			parts++;
		}
	}

	if( parts == 0 ) {
		goto cleanup;
	}

	/* encrypt the message, if possible; add Autocrypt:-header
	(encryption may modifiy the given object) */
	int e2ee_guaranteed = mrparam_get_int(msg->m_param, MRP_GUARANTEE_E2EE, 0);
	if( encrypt_to_self==0 || e2ee_guaranteed ) {
		/* we're here (1) _always_ on SMTP and (2) on IMAP _only_ if SMTP was encrypted before */
		mrmailbox_e2ee_encrypt(chat->m_mailbox, factory->m_recipients_addr, e2ee_guaranteed, encrypt_to_self, message, &e2ee_helper);
	}

	/* add a subject line */
	if( e2ee_helper.m_encryption_successfull ) {
		char* e = mrstock_str(MR_STR_ENCRYPTEDMSG); subject_str = mr_mprintf(MR_CHAT_PREFIX " %s", e); free(e);
		factory->m_out_encrypted = 1;
	}
	else {
		subject_str = get_subject(chat, msg, afwd_email);
	}
	struct mailimf_subject* subject = mailimf_subject_new(mr_encode_header_string(subject_str));
	mailimf_fields_add(imf_fields, mailimf_field_new(MAILIMF_FIELD_SUBJECT, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, subject, NULL, NULL, NULL));

	/* create the full mail and return */
	factory->m_out = mmap_string_new("");
	mailmime_write_mem(factory->m_out, &col, message);

	//{char* t4=mr_null_terminate(ret->str,ret->len); printf("MESSAGE:\n%s\n",t4);free(t4);}

	success = 1;

cleanup:
	if( message ) {
		mailmime_free(message);
	}
	mrmailbox_e2ee_thanks(&e2ee_helper); /* frees data referenced by "mailmime" but not freed by mailmime_free() */
	free(message_text); /* mailmime_set_body_text() does not take ownership of "text" */
	free(subject_str);
	free(afwd_email);
	return success;
}

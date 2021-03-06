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
 ******************************************************************************/


/* Purpose: Reading from IMAP servers with no dependencies to the database.
mrmailbox_t is only used for logging and to get information about
the online state. */


#ifndef __MRIMAP_H__
#define __MRIMAP_H__
#ifdef __cplusplus
extern "C" {
#endif


typedef struct mrloginparam_t mrloginparam_t;
typedef struct mrimap_t mrimap_t;

#define MR_IMAP_SEEN 0x0001L

typedef char*    (*mr_get_config_t)    (mrimap_t*, const char*, const char*);
typedef void     (*mr_set_config_t)    (mrimap_t*, const char*, const char*);
typedef void     (*mr_receive_imf_t)   (mrimap_t*, const char* imf_raw_not_terminated, size_t imf_raw_bytes, const char* server_folder, uint32_t server_uid, uint32_t flags);


/**
 * Library-internal.
 */
typedef struct mrimap_t
{
	/** @privatesection */

	char*                 m_imap_server;
	int                   m_imap_port;
	char*                 m_imap_user;
	char*                 m_imap_pw;
	int                   m_server_flags;

	int                   m_connected;
	mailimap*             m_hEtpan;   /* normally, if connected, m_hEtpan is also set; however, if a reconnection is required, we may lost this handle */

	time_t                m_last_fullread_time;

	int                   m_idle_set_up;
	char*                 m_selected_folder;
	int                   m_selected_folder_needs_expunge;
	int                   m_should_reconnect;

	int                   m_can_idle;
	int                   m_has_xlist;
	char*                 m_moveto_folder;// Folder, where reveived chat messages should go to.  Normally MR_CHATS_FOLDER, may be NULL to leave them in the INBOX
	char*                 m_sent_folder;  // Folder, where send messages should go to.  Normally MR_CHATS_FOLDER.
	char                  m_imap_delimiter;/* IMAP Path separator. Set as a side-effect in list_folders__ */

	pthread_cond_t        m_watch_cond;
	pthread_mutex_t       m_watch_condmutex;
	int                   m_watch_condflag;

	//time_t                m_enter_watch_wait_time;

	struct mailimap_fetch_type* m_fetch_type_uid;
	struct mailimap_fetch_type* m_fetch_type_message_id;
	struct mailimap_fetch_type* m_fetch_type_body;
	struct mailimap_fetch_type* m_fetch_type_flags;

	mr_get_config_t       m_get_config;
	mr_set_config_t       m_set_config;
	mr_receive_imf_t      m_receive_imf;
	void*                 m_userData;
	mrmailbox_t*          m_mailbox;

	int                   m_log_connect_errors;
	int                   m_skip_log_capabilities;

} mrimap_t;


mrimap_t* mrimap_new               (mr_get_config_t, mr_set_config_t, mr_receive_imf_t, void* userData, mrmailbox_t*);
void      mrimap_unref             (mrimap_t*);

int       mrimap_connect           (mrimap_t*, const mrloginparam_t*);
void      mrimap_disconnect        (mrimap_t*);
int       mrimap_is_connected      (mrimap_t*);
int       mrimap_fetch             (mrimap_t*);

void      mrimap_watch_n_wait      (mrimap_t*);
void      mrimap_interrupt_watch   (mrimap_t*);

int       mrimap_append_msg        (mrimap_t*, time_t timestamp, const char* data_not_terminated, size_t data_bytes, char** ret_server_folder, uint32_t* ret_server_uid);

#define   MR_MS_ALSO_MOVE          0x01
#define   MR_MS_SET_MDNSent_FLAG   0x02
#define   MR_MS_MDNSent_JUST_SET   0x10
int       mrimap_markseen_msg      (mrimap_t*, const char* folder, uint32_t server_uid, int ms_flags, char** ret_server_folder, uint32_t* ret_server_uid, int* ret_ms_flags); /* only returns 0 on connection problems; we should try later again in this case */

int       mrimap_delete_msg        (mrimap_t*, const char* rfc724_mid, const char* folder, uint32_t server_uid); /* only returns 0 on connection problems; we should try later again in this case */


#ifdef __cplusplus
} /* /extern "C" */
#endif
#endif /* __MRIMAP_H__ */


/*******************************************************************************
 *
 *                             Messenger Backend
 *     Copyright (C) 2016 Björn Petersen Software Design and Development
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
 * File:    mrmailbox.h
 * Authors: Björn Petersen
 * Purpose: MrMailbox represents a single mailbox, normally, typically is only
 *          one instance of this class present.
 *          Each mailbox is linked to an IMAP/POP3 account and uses a separate
 *          SQLite database for offline functionality and for mailbox-related
 *          settings.
 *
 ******************************************************************************/


#ifndef __MRMAILBOX_H__
#define __MRMAILBOX_H__


#include <libetpan/libetpan.h> // defines uint16_t etc.
#include "mrsqlite3.h"
#include "mrimap.h"
#include "mrerror.h"
#include "mrloginparam.h"
#include "mrmsg.h"


class MrChat;
class MrChatList;
class MrContact;


#define MR_VERSION_MAJOR    0
#define MR_VERSION_MINOR    1
#define MR_VERSION_REVISION 2


class MrMailbox
{
public:
	              MrMailbox            ();
	              ~MrMailbox           ();

	// open/close a mailbox object, if the given file does not exist, it is created
	// and can be set up using SetConfig() and Connect() afterwards.
	// sth. like "~/file" won't work on all systems, if in doubt, use absolute paths for dbfile.
	bool          Open                 (const char* dbfile);
	void          Close                ();

	// empty all tabes but leaves server configuration
	bool          Empty                ();

	// connect to the mailbox: error are be received asynchronously.
	bool          Connect              ();
	void          Disconnect           ();
	bool          Fetch                ();

	// iterate contacts
	size_t        GetContactCnt        ();
	MrContact*    GetContact           (size_t i);

	// iterate chats
	size_t        GetChatCnt           ();
	MrChatList*   GetChats             (); // the result must be delete'd
	MrChat*       GetChat              (const char* name); // the result must be delete'd
	MrChat*       GetChat              (uint32_t id); // the result must be delete'd

	// handle configurations
	bool          SetConfig            (const char* key, const char* value) { MrSqlite3Locker l(m_sql); return m_sql.SetConfig(key, value); }
	char*         GetConfig            (const char* key, const char* def)   { MrSqlite3Locker l(m_sql); return m_sql.GetConfig(key, def); } // the returned string must be free()'d, returns NULL on errors
	int32_t       GetConfigInt         (const char* key, int32_t def)       { MrSqlite3Locker l(m_sql); return m_sql.GetConfigInt(key, def); }

	// misc
	char*         GetDbFile            (); // the returned string must be free()'d, returns NULL on errors or if no database is open
	char*         GetInfo              (); // multi-line output; the returned string must be free()'d, returns NULL on errors

	// data, should be treated as read-only
	MrSqlite3     m_sql;

private:
	// private stuff
	MrLoginParam  m_loginParam;
	MrImap        m_imap;

	// when fetching messages, this normally results in calls to ReceiveImf().
	// CAVE: ReceiveImf() may be called from within a working thread!
	void          ReceiveImf           (const char* imf, size_t imf_len);

	friend class  MrImap;
};


#endif // __MRMAILBOX_H__

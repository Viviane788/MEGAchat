#include "contactList.h"
#include "ITypes.h" //for IPtr
#ifdef _WIN32
    #include <winsock2.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mstrophepp.h>
#include "rtcModule/IRtcModule.h"
#include "dummyCrypto.h" //for makeRandomString
#include "strophe.disco.h"
#include "base/services.h"
#include "sdkApi.h"
#include "megaCryptoFunctions.h"
#include <serverListProvider.h>
#include <memory>
#include "chatClient.h"
#include "textModule.h"
#include <chatd.h>
#include <db.h>
#include <buffer.h>
#include <chatdDb.h>
#include <megaapi_impl.h>

#define _QUICK_LOGIN_NO_RTC
using namespace promise;

namespace karere
{
void Client::sendPong(const std::string& peerJid, const std::string& messageId)
{
    strophe::Stanza pong(*conn);
    pong.setAttr("type", "result")
        .setAttr("to", peerJid)
        .setAttr("from", conn->fullJid())
        .setAttr("id", messageId);

    conn->send(pong);
}


Client::Client(IGui& aGui, const char* homedir)
 :mAppDir(checkAppDir(homedir)), db(openDb()), conn(new strophe::Connection(services_strophe_get_ctx())),
  api(new MyMegaApi("karere-native")), userAttrCache(*this), gui(aGui),
  mXmppContactList(*this),
  mXmppServerProvider(new XmppServerProvider("https://gelb530n001.karere.mega.nz", "xmpp", KARERE_FALLBACK_XMPP_SERVERS))
{
    SqliteStmt stmt(db, "select value from vars where name='my_handle'");
    if (!stmt.step())
    {
        KR_LOG_DEBUG("No own mega handle found in local db");
        return;
    }
    auto handle = stmt.uint64Col(0);
    if (handle == 0 || handle == mega::UNDEF)
        throw std::runtime_error("Invalid own handle in local database");

    mMyHandle = handle;
    chatd.reset(new chatd::Client(mMyHandle));
    contactList.reset(new ContactList(*this));
    chats.reset(new ChatRoomList(*this));
}

std::string Client::checkAppDir(const char* dir)
{
    std::string path;
    if (dir)
    {
        path = dir;
    }
    else
    {
        const char* homedir = getenv(
            #ifndef _WIN32
                    "HOME"
            #else
                    "HOMEPATH"
            #endif
        );
        if (!homedir)
            throw std::runtime_error("Cant get HOME env variable");
        path = homedir;
        path.append("/.karere");
    }
    struct stat info;
    auto ret = stat(path.c_str(), &info);
    if (ret == 0)
    {
        if ((info.st_mode & S_IFDIR) == 0)
            throw std::runtime_error("Application directory path is taken by a file");
    }
    else
    {
        ret = mkdir(path.c_str(), 0700);
        if (ret)
        {
            char buf[512];
#ifdef _WIN32
            strerror_s(buf, 511, ret);
#else
            strerror_r(ret, buf, 511);
#endif
            buf[511] = 0; //just in case
            throw std::runtime_error(std::string("Error creating application directory: ")+buf);
        }
    }
    return path;
}
sqlite3* Client::openDb()
{
    sqlite3* database = nullptr;
    std::string path = mAppDir+"/karere.db";
    int ret = sqlite3_open(path.c_str(), &database);
    if (ret != SQLITE_OK || !database)
        throw std::runtime_error("Can't access application database at "+path);
    return database;
}


Client::~Client()
{
    //when the strophe::Connection is destroyed, its handlers are automatically destroyed
}

#define TOKENPASTE2(a,b) a##b
#define TOKENPASTE(a,b) TOKENPASTE2(a,b)

#define SHARED_STATE(varname, membtype)             \
    struct TOKENPASTE(SharedState,__LINE__){membtype value;};  \
    std::shared_ptr<TOKENPASTE(SharedState, __LINE__)> varname(new TOKENPASTE(SharedState,__LINE__))

promise::Promise<int> Client::init()
{
    SqliteStmt stmt(db, "select value from vars where name='sid'");
    std::string sid = stmt.step() ? stmt.stringCol(0) : std::string();
    ApiPromise pmsMegaLogin;
    if (sid.empty())
    {
        std::string mail, pass;
        if (!gui.requestLoginCredentials(mail, pass))
            return promise::Error("No local session cache, and no login credential provided by user");
        pmsMegaLogin = api->call(&mega::MegaApi::login, mail.c_str(), pass.c_str());
    }
    else
    {
        mHadSid = true;
        pmsMegaLogin = api->call(&mega::MegaApi::fastLogin, sid.c_str())
        .then([this](ReqResult result)
        {
            mHadSid = true;
            return result;
        });
    }
    pmsMegaLogin.then([this](ReqResult result) mutable
    {
        mIsLoggedIn = true;
        KR_LOG_DEBUG("Login to Mega API successful");
//        userAttrCache.onLogin();
        SdkString uh = api->getMyUserHandle();
        if (!uh.c_str() || !uh.c_str()[0])
            throw std::runtime_error("Could not get our own user handle from API");
        chatd::Id handle(uh.c_str());
        if (mMyHandle != mega::UNDEF)
        {
            if (handle != mMyHandle)
                throw std::runtime_error("Local DB inconsistency: Own userhandle returned from API differs from the one in local db");
        }
        else
        {
            KR_LOG_DEBUG("Obtained our own handle (%s), recording to database", handle.toString().c_str());
            mMyHandle = handle;
            sqliteQuery(db, "insert or replace into vars(name,value) values('my_handle', ?)", mMyHandle);
            chatd.reset(new chatd::Client(mMyHandle));
            contactList.reset(new ContactList(*this));
            chats.reset(new ChatRoomList(*this));
        }
//        if (onChatdReady)
//            onChatdReady();

        SdkString xmppPass = api->dumpXMPPSession();
        if (xmppPass.size() < 16)
            throw std::runtime_error("Mega session id is shorter than 16 bytes");
        ((char&)xmppPass.c_str()[16]) = 0;

        //xmpp_conn_set_keepalive(*conn, 10, 4);
        // setup authentication information
        std::string jid = std::string(api->getMyXMPPJid())+"@" KARERE_XMPP_DOMAIN "/kn_";
        jid.append(rtcModule::makeRandomString(10));
        xmpp_conn_set_jid(*conn, jid.c_str());
        xmpp_conn_set_pass(*conn, xmppPass.c_str());
        KR_LOG_DEBUG("xmpp user = '%s', pass = '%s'", jid.c_str(), xmppPass.c_str());
        setupHandlers();
    });
    auto pmsMegaGud = pmsMegaLogin.then([this](ReqResult result)
    {
        return api->call(&mega::MegaApi::getUserData);
    })
    .then([this](ReqResult result)
    {
        api->userData = result;
    });
    auto pmsMegaFetch = pmsMegaLogin.then([this](ReqResult result)
    {
        return api->call(&mega::MegaApi::fetchNodes);
    })
    .then([this](ReqResult result)
    {
        if (!mHadSid)
        {
            const char* sid = api->dumpSession();
            sqliteQuery(db, "insert or replace into vars(name,value) values('sid',?)", sid);
        }

        userAttrCache.onLogin();
        userAttrCache.getAttr(mMyHandle, mega::MegaApi::USER_ATTR_LASTNAME, this,
        [](Buffer* buf, void* userp)
        {
            if (buf)
                static_cast<Client*>(userp)->mMyName = buf->buf()+1;
        });
        contactList->syncWithApi(*api->getContacts());
        return api->call(&mega::MegaApi::fetchChats);
    })
    .then([this](ReqResult result)
    {
        auto chatRooms = result->getMegaTextChatList();
        if (chatRooms)
        {
            chats->syncRoomsWithApi(*chatRooms);
        }
    });

    SHARED_STATE(server, std::shared_ptr<HostPortServerInfo>);
    auto pmsGelbReq = mXmppServerProvider->getServer()
    .then([server](std::shared_ptr<HostPortServerInfo> aServer) mutable
    {
        server->value = aServer;
        return 0;
    });
    return promise::when(pmsMegaGud, pmsMegaFetch, pmsGelbReq)
    .then([this, server]()
    {
        if (onChatdReady)
            onChatdReady();

// initiate connection
        return mega::retry([this, server](int no) -> promise::Promise<void>
        {
            if (no < 2)
            {
                return mega::performWithTimeout([this, server]()
                {
                    KR_LOG_INFO("Connecting to xmpp server %s...", server->value->host.c_str());
                    return conn->connect(server->value->host.c_str(), 0);
                }, KARERE_LOGIN_TIMEOUT,
                [this]()
                {
                    xmpp_disconnect(*conn, -1);
                });
            }
            else
            {
                return mXmppServerProvider->getServer()
                .then([this](std::shared_ptr<HostPortServerInfo> aServer)
                {
                    KR_LOG_WARNING("Connecting to new xmpp server: %s...", aServer->host.c_str());
                    return mega::performWithTimeout([this, aServer]()
                    {
                        return conn->connect(aServer->host.c_str(), 0);
                    }, KARERE_LOGIN_TIMEOUT,
                    [this]()
                    {
                        xmpp_disconnect(*conn, -1);
                    });
                });
            }
        }, nullptr, 0, 0, KARERE_RECONNECT_DELAY_MAX, KARERE_RECONNECT_DELAY_INITIAL);
    })
    .then([this]()
    {
        KR_LOG_INFO("Login success");

// handle reconnect due to network errors
        setupReconnectHandler();
// create and register disco strophe plugin
        conn->registerPlugin("disco", new disco::DiscoPlugin(*conn, "Karere Native"));

// Create and register the rtcmodule plugin
// the MegaCryptoFuncs object needs api->userData (to initialize the private key etc)
// To use DummyCrypto: new rtcModule::DummyCrypto(jid.c_str());
        rtc = rtcModule::create(*conn, this, new rtcModule::MegaCryptoFuncs(*this), KARERE_DEFAULT_TURN_SERVERS);
        conn->registerPlugin("rtcmodule", rtc);

// create and register text chat plugin
        mTextModule = new TextModule(*this);
        conn->registerPlugin("textchat", mTextModule);
        KR_LOG_DEBUG("webrtc and textchat plugins initialized");

// install contactlist handlers before sending initial presence so that the presences that start coming after that get processed
        auto pms = mXmppContactList.init();
// Send initial <presence/> so that we appear online to contacts
        strophe::Stanza pres(*conn);
        pres.setName("presence");
        conn->send(pres);
        return pms;
    })
    .then([this]()
    {
        KR_LOG_DEBUG("Contactlist initialized");
        //startKeepalivePings();
        return 0;
    })
    .fail([](const promise::Error& err)
    {
        KR_LOG_ERROR("Error initializing client: %s", err.what());
        return err;
    });
}

void Client::setupHandlers()
{
    conn->addHandler([this](strophe::Stanza stanza, void*, bool &keep) mutable
    {
            sendPong(stanza.attr("from"), stanza.attr("id"));
    }, "urn::xmpp::ping", "iq", nullptr, nullptr);

}

void Client::setupReconnectHandler()
{
    mReconnectController.reset(mega::createRetryController(
    [this](int no)
    {
        mLastPingTs = 0;
        return conn->connect(KARERE_DEFAULT_XMPP_SERVER, 0);
    },
    [this]()
    {
        xmpp_disconnect(*conn, -1);
    }, KARERE_LOGIN_TIMEOUT, 0, KARERE_RECONNECT_DELAY_MAX, KARERE_RECONNECT_DELAY_INITIAL));

    mReconnectConnStateHandler = conn->addConnStateHandler(
       [this](xmpp_conn_event_t event, int error,
        xmpp_stream_error_t* stream_error, bool& keepHandler) mutable
    {
        if (((event != XMPP_CONN_DISCONNECT) && (event != XMPP_CONN_FAIL)) || isTerminating)
            return;
        assert(xmpp_conn_get_state(*conn) == XMPP_STATE_DISCONNECTED);
        if (mReconnectController->state() & mega::rh::kStateBitRunning)
            return;

        if (mReconnectController->state() == mega::rh::kStateFinished) //we had previous retry session, reset the retry controller
            mReconnectController->reset();
        mReconnectController->start(500); //need to process(i.e. ignore) all stale libevent messages for the old connection so they don't get interpreted in the context of the new connection
    });
#if 0
    //test
    mega::setInterval([this]()
    {
        printf("simulating disconnect\n");
        xmpp_disconnect(*conn, -1);
    }, 6000);
#endif
}


void Client::notifyNetworkOffline()
{
    KR_LOG_WARNING("Network offline notification received, starting reconnect attempts");
    if (xmpp_conn_get_state(*conn) == XMPP_STATE_DISCONNECTED)
    {
        //if we are disconnected, the retry controller must never be at work, so not 'finished'
        assert(mReconnectController->state() != mega::rh::kStateFinished);
        if (mReconnectController->currentAttemptNo() > 2)
            mReconnectController->restart();
    }
    else
    {
        conn->disconnect(-1); //this must trigger the conn state handler which will start the reconnect controller
    }
}


void Client::notifyNetworkOnline()
{
    if (xmpp_conn_get_state(*conn) == XMPP_STATE_CONNECTED)
        return;

    if (mReconnectController->state() == mega::rh::kStateFinished)
    {
        KR_LOG_WARNING("notifyNetworkOnline: reconnect controller is in 'finished' state, but connection is not connected. Resetting reconnect controller.");
        mReconnectController->reset();
    }
    mReconnectController->restart();
}

promise::Promise<void> Client::terminate()
{
    if (isTerminating)
    {
        KR_LOG_WARNING("Client::terminate: Already terminating");
        return promise::Promise<void>();
    }
    isTerminating = true;
    if (mReconnectConnStateHandler)
    {
        conn->removeConnStateHandler(mReconnectConnStateHandler);
        mReconnectConnStateHandler = 0;
    }
    if (mReconnectController)
        mReconnectController->abort();
    if (rtc)
        rtc->hangupAll();
    sqlite3_close(db);
    promise::Promise<void> pms;
    conn->disconnect(2000)
    //resolve output promise asynchronously, because the callbacks of the output
    //promise may free the client, and the resolve()-s of the input promises
    //(mega and conn) are within the client's code, so any code after the resolve()s
    //that tries to access the client will crash
    .then([this, pms](int) mutable
    {
        return api->call(&::mega::MegaApi::localLogout);
    })
    .then([pms](ReqResult result)
    {
        mega::marshallCall([pms]() mutable { pms.resolve(); });
    })
    .fail([pms](const promise::Error& err) mutable
    {
        mega::marshallCall([pms, err]() mutable { pms.reject(err); });
        return err;
    });
    return pms;
}

void Client::startKeepalivePings()
{
    mega::setInterval([this]()
    {
        if (!xmpp_conn_is_authenticated(*conn))
            return;
        if (mLastPingTs) //waiting for pong
        {
            if (xmpp_time_stamp()-mLastPingTs > 9000)
            {
                KR_LOG_WARNING("Keepalive ping timeout");
                notifyNetworkOffline();
            }
        }
        else
        {
            mLastPingTs = xmpp_time_stamp();
            pingPeer(nullptr)
            .then([this](strophe::Stanza s)
            {
                mLastPingTs = 0;
                return 0;
            });
        }
    }, 10000);
}


strophe::StanzaPromise Client::pingPeer(const char* peerJid)
{
    strophe::Stanza ping(*conn);
    ping.setName("iq")
        .setAttr("type", "get")
        .c("ping")
                .setAttr("xmlns", "urn:xmpp:ping");
    if (peerJid)
        ping.setAttr("to", peerJid);

    return conn->sendIqQuery(ping, "png")
    .fail([](const promise::Error& err)
    {
        KR_LOG_ERROR("Error receiving pong\n");
        return err;
    });
}

void Client::setPresence(Presence pres, const int delay)
{
    strophe::Stanza msg(*conn);
    msg.setName("presence")
       .setAttr("id", generateMessageId(std::string("presence"), std::string("")))
       .c("show")
           .t(pres.toString())
           .up()
       .c("status")
           .t(pres.toString())
           .up();

    if(delay > 0)
    {
        msg.c("delay")
                .setAttr("xmlns", "urn:xmpp:delay")
                .setAttr("from", conn->fullJid());
    }
    conn->send(msg);
}


promise::Promise<message_bus::SharedMessage<M_MESS_PARAMS>>
Client::getOtherUserInfo(std::string &emailAddress)
{
    std::string event(USER_INFO_EVENT);
    event.append(emailAddress);
    message_bus::SharedMessage<M_MESS_PARAMS> userMessage(event);

    return api->call(&mega::MegaApi::getUserData, emailAddress.c_str())
    .then([this, userMessage](ReqResult result)
    {
        //const char *peer = result->getText();
        //const char *pk = result->getPassword();

        return userMessage;
    });
/*
 * //av: cant return nullptr instead of SharedMessage. This works only for a class with constructor that can take nullptr as argument, and the ctor is not marked as explicit
   .fail([&](const promise::Error &err)
    {
        return nullptr;
    });
*/
}
UserAttrDesc attrDesc[mega::MegaApi::USER_ATTR_LAST_INTERACTION+1] =
{ //getData func | changeMask
  //0 - avatar
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getFile(), strlen(req.getFile())); }, mega::MegaUser::CHANGE_TYPE_AVATAR},
  //firstname and lastname are handled specially, so we don't use a descriptor for it
  //1 - first name
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getText(), strlen(req.getText())); }, mega::MegaUser::CHANGE_TYPE_FIRSTNAME},
  //2 = last name
   { [](const mega::MegaRequest& req)->Buffer* { return new Buffer(req.getText(), strlen(req.getText())); }, mega::MegaUser::CHANGE_TYPE_LASTNAME},
  //keyring
   { [](const mega::MegaRequest& req)->Buffer* { throw std::runtime_error("not implemented"); }, mega::MegaUser::CHANGE_TYPE_AUTH},
  //last interaction
   { [](const mega::MegaRequest& req)->Buffer* { throw std::runtime_error("not implemented"); }, mega::MegaUser::CHANGE_TYPE_LSTINT}
};

UserAttrCache::~UserAttrCache()
{
    mClient.api->removeGlobalListener(this);
}

void UserAttrCache::dbWrite(const UserAttrPair& key, const Buffer& data)
{
    sqliteQuery(mClient.db,
        "insert or replace into userattrs(userid, type, data) values(?,?,?)",
        key.user, key.attrType, data);
}

UserAttrCache::UserAttrCache(Client& aClient): mClient(aClient)
{
    //load only api-supported types, skip 'virtual' types >= 128 as they can't be fetched in the normal way
    SqliteStmt stmt(mClient.db, "select userid, type, data from userattrs where type < 128");
    while(stmt.step())
    {
        std::unique_ptr<Buffer> data(new Buffer((size_t)sqlite3_column_bytes(stmt, 2)));
        stmt.blobCol(2, *data);

        emplace(std::piecewise_construct,
            std::forward_as_tuple(stmt.uint64Col(0), stmt.intCol(1)),
            std::forward_as_tuple(std::make_shared<UserAttrCacheItem>(data.release(), false)));
    }
    mClient.api->addGlobalListener(this);
}

void UserAttrCache::onUsersUpdate(mega::MegaApi* api, mega::MegaUserList *users)
{
    if (!users)
        return;
    std::shared_ptr<mega::MegaUserList> copy(users->copy());
    mega::marshallCall([this, copy]() { onUserAttrChange(*copy);});
}

void UserAttrCache::onUserAttrChange(mega::MegaUserList& users)
{
    for (auto i=0; i<users.size(); i++)
    {
        auto& user = *users.get(i);
        int changed = user.getChanges();
        for (auto t = 0; t <= mega::MegaApi::USER_ATTR_LAST_INTERACTION; t++)
        {
            if ((changed & attrDesc[t].changeMask) == 0)
                continue;
            UserAttrPair key(user.getHandle(), t);
            auto it = find(key);
            if (it == end()) //we don't have such attribute
                continue;
            auto& item = it->second;
            dbInvalidateItem(key); //immediately invalidate parsistent cache
            if (item->cbs.empty()) //we aren't using that item atm
            { //delete it from memory as well, forcing it to be freshly fetched if it's requested
                erase(key);
                continue;
            }
            if (item->pending)
                continue;
            item->pending = kCacheFetchUpdatePending;
            fetchAttr(key, item);
        }
    }
}
void UserAttrCache::dbInvalidateItem(const UserAttrPair& key)
{
    sqliteQuery(mClient.db, "delete from userattrs where userid=? and type=?",
                key.user, key.attrType);
}

void UserAttrCacheItem::notify()
{
    for (auto it=cbs.begin(); it!=cbs.end();)
    {
        auto curr = it;
        it++;
        curr->cb(data, curr->userp); //may erase curr
    }
}
UserAttrCacheItem::~UserAttrCacheItem()
{
    if (data)
        delete data;
}

uint64_t UserAttrCache::addCb(iterator itemit, UserAttrReqCbFunc cb, void* userp)
{
    auto& cbs = itemit->second->cbs;
    auto it = cbs.emplace(cbs.end(), cb, userp);
    mCallbacks.emplace(std::piecewise_construct, std::forward_as_tuple(++mCbId),
                       std::forward_as_tuple(itemit, it));
    return mCbId;
}

bool UserAttrCache::removeCb(const uint64_t& cbid)
{
    auto it = mCallbacks.find(cbid);
    if (it == mCallbacks.end())
        return false;
    auto& cbDesc = it->second;
    cbDesc.itemit->second->cbs.erase(cbDesc.cbit);
    return true;
}

uint64_t UserAttrCache::getAttr(const uint64_t& userHandle, unsigned type,
            void* userp, UserAttrReqCbFunc cb)
{
    UserAttrPair key(userHandle, type);
    auto it = find(key);
    if (it != end())
    {
        auto& item = *it->second;
        if (cb)
        { //TODO: not optimal to store each cb pointer, as these pointers would be mostly only a few, with different userp-s
            auto cbid = addCb(it, cb, userp);
            if (item.pending != kCacheFetchNewPending)
                cb(item.data, userp);
            return cbid;
        }
        else
        {
            return 0;
        }
    }

    auto item = std::make_shared<UserAttrCacheItem>(nullptr, kCacheFetchNewPending);
    it = emplace(key, item).first;
    uint64_t cbid = cb ? addCb(it, cb, userp) : 0;
    fetchAttr(key, item);
    return cbid;
}
void UserAttrCache::fetchAttr(const UserAttrPair& key, std::shared_ptr<UserAttrCacheItem>& item)
{
    if (!mClient.isLoggedIn())
        return;
    if (key.attrType != mega::MegaApi::USER_ATTR_LASTNAME)
    {
        auto& attrType = attrDesc[key.attrType];
        mClient.api->call(&mega::MegaApi::getUserAttribute,
            base64urlencode(&key.user, sizeof(key.user)).c_str(), (int)key.attrType)
        .then([this, &attrType, key, item](ReqResult result)
        {
            item->pending = kCacheFetchNotPending;
            item->data = attrType.getData(*result);
            dbWrite(key, *item->data);
            item->notify();
        })
        .fail([this, item](const promise::Error& err)
        {
            item->pending = kCacheFetchNotPending;
            item->data = nullptr;
            item->notify();
            return err;
        });
    }
    else
    {
        item->data = new Buffer;
        std::string strUh = base64urlencode(&key.user, sizeof(key.user));
        mClient.api->call(&mega::MegaApi::getUserAttribute, strUh.c_str(),
                  (int)MyMegaApi::USER_ATTR_FIRSTNAME)
        .then([this, strUh, key, item](ReqResult result)
        {
            const char* name = result->getText();
            if (!name)
                name = "(null)";
            size_t len = strlen(name);
            if (len > 255)
            {
                item->data->append<unsigned char>(255);
                item->data->append(name, 252);
                item->data->append("...", 3);
            }
            else
            {
                item->data->append<unsigned char>(len);
                item->data->append(name);
            }
            return mClient.api->call(&mega::MegaApi::getUserAttribute, strUh.c_str(),
                    (int)MyMegaApi::USER_ATTR_LASTNAME);
        })
        .then([this, key, item](ReqResult result)
        {
            Buffer* data = item->data;
            data->append(' ');
            const char* name = result->getText();
            data->append(name ? name : "(null)").append<char>(0);
            item->pending = kCacheFetchNotPending;
            dbWrite(key, *data);
            item->notify();
        })
        .fail([this, item](const promise::Error& err)
        {
            item->data = nullptr;
            item->pending = kCacheFetchNotPending;
            item->notify();
        });
    }
}

void UserAttrCache::onLogin()
{
    for (auto& item: *this)
    {
        if (item.second->pending != kCacheFetchNotPending)
            fetchAttr(item.first, item.second);
    }
}

promise::Promise<Buffer*> UserAttrCache::getAttr(const uint64_t &user, unsigned attrType)
{
    struct State
    {
        Promise<Buffer*> pms;
        UserAttrCache* self;
        uint64_t cbid;
    };
    State* state = new State;
    state->self = this;
    state->cbid = getAttr(user, attrType, state, [](Buffer* buf, void* userp)
    {
        auto s = static_cast<State*>(userp);
        s->self->removeCb(s->cbid);
        if (buf)
            s->pms.resolve(buf);
        else
            s->pms.reject("failed");
        delete s;
    });
    return state->pms;
}

promise::Promise<message_bus::SharedMessage<M_MESS_PARAMS>>
Client::getThisUserInfo()
{
    std::string event(THIS_USER_INFO_EVENT);
    message_bus::SharedMessage<M_MESS_PARAMS> userMessage(event);

    return api->call(&mega::MegaApi::getUserData)
    .then([this, userMessage](ReqResult result)
    {
        return userMessage; //av: was nullptr, but compile error - Promise<return type of this> must match function return type
    })
    .fail([&](const promise::Error &err)
    {
        return userMessage; //av: same here - was nullptr but compile error
    });
}

ChatRoom::ChatRoom(ChatRoomList& aParent, const uint64_t& chatid, bool aIsGroup, const std::string& aUrl, unsigned char aShard,
  char aOwnPriv)
:parent(aParent), mChatid(chatid), mUrl(aUrl), mShardNo(aShard), mIsGroup(aIsGroup), mOwnPriv(aOwnPriv)
{}

void ChatRoom::join()
{
    parent.client.chatd->join(mChatid, mShardNo, mUrl, *this);
}

GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const uint64_t& chatid, const std::string& aUrl, unsigned char aShard,
    char aOwnPriv, const std::string& title)
:ChatRoom(parent, chatid, true, aUrl, aShard, aOwnPriv), mTitleString(title),
  mHasUserTitle(!title.empty())
{
    SqliteStmt stmt(parent.client.db, "select userid, priv from chat_peers where chatid=?");
    stmt << mChatid;
    while(stmt.step())
    {
        addMember(stmt.uint64Col(0), stmt.intCol(1), false);
    }
    mTitleDisplay = parent.client.gui.contactList().createGroupChatItem(*this);
    if (!mTitleString.empty())
        mTitleDisplay->updateTitle(mTitleString);
    join();
}
PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const uint64_t& chatid, const std::string& aUrl,
    unsigned char aShard, char aOwnPriv, const uint64_t& peer, char peerPriv)
:ChatRoom(parent, chatid, false, aUrl, aShard, aOwnPriv), mPeer(peer), mPeerPriv(peerPriv)
{
    mTitleDisplay = parent.client.contactList->attachRoomToContact(peer, *this);
    join();
}

PeerChatRoom::PeerChatRoom(ChatRoomList& parent, const mega::MegaTextChat& chat)
:ChatRoom(parent, chat.getHandle(), false, chat.getUrl(), chat.getShard(), chat.getOwnPrivilege()),
    mPeer((uint64_t)-1), mPeerPriv(0)
{
    assert(!chat.isGroup());
    auto peers = chat.getPeerList();
    assert(peers);
    assert(peers->size() == 1);
    mPeer = peers->getPeerHandle(0);
    mPeerPriv = peers->getPeerPrivilege(0);

    sqliteQuery(parent.client.db, "insert into chats(chatid, url, shard, peer, peer_priv, own_priv) values (?,?,?,?,?,?)",
        mChatid, mUrl, mShardNo, mPeer, mPeerPriv, mOwnPriv);
//just in case
    sqliteQuery(parent.client.db, "delete from chat_peers where chatid = ?", mChatid);
    mTitleDisplay = parent.client.contactList->attachRoomToContact(mPeer, *this);
    KR_LOG_DEBUG("Added 1on1 chatroom '%s' from API", chatd::Id(mChatid).toString().c_str());
    join();
}

void PeerChatRoom::syncOwnPriv(char priv)
{
    if (mOwnPriv == priv)
        return;

    mOwnPriv = priv;
    sqliteQuery(parent.client.db, "update chats set own_priv = ? where chatid = ?",
                priv, mChatid);
}

void PeerChatRoom::syncPeerPriv(char priv)
{
    if (mPeerPriv == priv)
        return;
    mPeerPriv = priv;
    sqliteQuery(parent.client.db, "update chats set peer_priv = ? where chatid = ?",
                priv, mChatid);
}
void PeerChatRoom::syncWithApi(const mega::MegaTextChat &chat)
{
    ChatRoom::syncRoomPropertiesWithApi(chat);
    syncOwnPriv(chat.getOwnPrivilege());
    syncPeerPriv(chat.getPeerList()->getPeerPrivilege(0));
}
static std::string sEmptyString;
const std::string& PeerChatRoom::titleString() const
{
    return mContact ? mContact->titleString(): sEmptyString;
}

void GroupChatRoom::addMember(const uint64_t& userid, char priv, bool saveToDb)
{
    auto it = mPeers.find(userid);
    if (it != mPeers.end())
    {
        if (it->second->mPriv == priv)
        {
            saveToDb = false;
        }
        else
        {
            it->second->mPriv = priv;
        }
    }
    else
    {
        mPeers.emplace(userid, new Member(*this, userid, priv)); //usernames will be updated when the Member object gets the username attribute
    }
    if (saveToDb)
    {
        sqliteQuery(parent.client.db, "insert or replace into chat_peers(chatid, userid, priv) values(?,?,?)",
            mChatid, userid, priv);
    }
}
bool GroupChatRoom::removeMember(const uint64_t& userid)
{
    auto it = mPeers.find(userid);
    if (it == mPeers.end())
    {
        KR_LOG_WARNING("GroupChatRoom::removeMember for a member that we don't have, ignoring");
        return false;
    }
    delete it->second;
    mPeers.erase(it);
    sqliteQuery(parent.client.db, "delete from chat_peers where chatid=? and userid=?",
                mChatid, userid);
    updateTitle();
    return true;
}

void GroupChatRoom::deleteSelf()
{
    auto db = parent.client.db;
    sqliteQuery(db, "delete from chat_peers where chatid=?", mChatid);
    sqliteQuery(db, "delete from chats where chatid=?", mChatid);
    delete this;
}

ChatRoomList::ChatRoomList(Client& aClient)
:client(aClient)
{
    loadFromDb();
    client.api->addGlobalListener(this);
}

void ChatRoomList::loadFromDb()
{
    SqliteStmt stmt(client.db, "select chatid, url, shard, own_priv, peer, peer_priv, title from chats");
    while(stmt.step())
    {
        auto chatid = stmt.uint64Col(0);
        if (find(chatid) != end())
        {
            KR_LOG_WARNING("ChatRoomList: Attempted to load from db cache a chatid that is already in memory");
            continue;
        }
        auto peer = stmt.uint64Col(4);
        ChatRoom* room;
        if (peer != uint64_t(-1))
            room = new PeerChatRoom(*this, chatid, stmt.stringCol(1), stmt.intCol(2), stmt.intCol(3), peer, stmt.intCol(5));
        else
            room = new GroupChatRoom(*this, chatid, stmt.stringCol(1), stmt.intCol(2), stmt.intCol(3), stmt.stringCol(6));
        emplace(chatid, room);
    }
}
void ChatRoomList::syncRoomsWithApi(const mega::MegaTextChatList& rooms)
{
    auto size = rooms.size();
    for (int i=0; i<size; i++)
    {
        addRoom(*rooms.get(i));
    }
}
ChatRoom& ChatRoomList::addRoom(const mega::MegaTextChat& room, const std::string& groupUserTitle)
{
    auto chatid = room.getHandle();
    auto it = find(chatid);
    if (it != end()) //we already have that room
    {
        it->second->syncWithApi(room);
        return *it->second;
    }
    ChatRoom* ret;
    if(room.isGroup())
        ret = new GroupChatRoom(*this, room, groupUserTitle); //also writes it to cache
    else
        ret = new PeerChatRoom(*this, room);
    emplace(chatid, ret);
    return *ret;
}
bool ChatRoomList::removeRoom(const uint64_t &chatid)
{
    auto it = find(chatid);
    if (it == end())
        return false;
    if (!it->second->isGroup())
        throw std::runtime_error("Can't delete a 1on1 chat");
    static_cast<GroupChatRoom*>(it->second)->deleteSelf();
    erase(it);
    return true;
}

void ChatRoomList::onChatsUpdate(mega::MegaApi*, mega::MegaTextChatList* rooms)
{
    printf("onChatsUpdated\n");
//    syncRoomsWithApi(*rooms);
}

ChatRoomList::~ChatRoomList()
{
    for (auto& room: *this)
        delete room.second;
}

GroupChatRoom::GroupChatRoom(ChatRoomList& parent, const mega::MegaTextChat& chat, const std::string &userTitle)
:ChatRoom(parent, chat.getHandle(), true, chat.getUrl(), chat.getShard(), chat.getOwnPrivilege()),
  mTitleString(userTitle), mHasUserTitle(!userTitle.empty())
{
    auto peers = chat.getPeerList();
    assert(peers);
    auto size = peers->size();
    for (int i=0; i<size; i++)
    {
        auto handle = peers->getPeerHandle(i);
        mPeers[handle] = new Member(*this, handle, peers->getPeerPrivilege(i)); //may try to access mTitleDisplay, but we have set it to nullptr, so it's ok
    }
//save to db
    auto db = karere::gClient->db;
    sqliteQuery(db, "delete from chat_peers where chatid=?", mChatid);
    if (!userTitle.empty())
    {
        sqliteQuery(db, "insert or replace into chats(chatid, url, shard, peer, peer_priv, own_priv, title) values(?,?,?,-1,0,?,?)",
                mChatid, mUrl, mShardNo, mOwnPriv, userTitle);
    }
    else
    {
        sqliteQuery(db, "insert or replace into chats(chatid, url, shard, peer, peer_priv, own_priv) values(?,?,?,-1,0,?)",
                mChatid, mUrl, mShardNo, mOwnPriv);
        loadUserTitle();
    }
    SqliteStmt stmt(db, "insert into chat_peers(chatid, userid, priv) values(?,?,?)");
    for (auto& m: mPeers)
    {
        stmt << mChatid << m.first << m.second->mPriv;
        stmt.step();
        stmt.reset();
    }
    mTitleDisplay = parent.client.gui.contactList().createGroupChatItem(*this);
    if (!mTitleString.empty())
        mTitleDisplay->updateTitle(mTitleString);
    join();
}

void GroupChatRoom::loadUserTitle()
{
    //load user title if set
    SqliteStmt stmt(parent.client.db, "select title from chats where chatid = ?");
    stmt << mChatid;
    if (!stmt.step())
    {
        mHasUserTitle = false;
        return;
    }
    std::string strTitle = stmt.stringCol(0);
    if (strTitle.empty())
    {
        mHasUserTitle = false;
        return;
    }
    mTitleString = strTitle;
    mHasUserTitle = true;
}

void GroupChatRoom::setUserTitle(const std::string& title)
{
    mTitleString = title;
    if (mTitleString.empty())
    {
        mHasUserTitle = false;
        sqliteQuery(parent.client.db, "update chats set title=NULL where chatid=?", mChatid);
    }
    else
    {
        mHasUserTitle = true;
        sqliteQuery(parent.client.db, "update chats set title=? where chatid=?", mTitleString, mChatid);
    }
}

GroupChatRoom::~GroupChatRoom()
{
    parent.client.chatd->leave(mChatid);
    for (auto& m: mPeers)
        delete m.second;
    parent.client.gui.contactList().removeGroupChatItem(mTitleDisplay);
}

promise::Promise<void> GroupChatRoom::leave()
{
    return parent.client.api->call(&mega::MegaApi::removeFromChat, mChatid, parent.client.myHandle())
    .then([this](ReqResult result)
    {
        parent.removeRoom(mChatid); //this should clear all references to the chatd Messages object
    });
}

promise::Promise<void> GroupChatRoom::invite(uint64_t userid, char priv)
{
    return parent.client.api->call(&mega::MegaApi::inviteToChat, mChatid, userid, priv)
    .then([this, userid, priv](ReqResult)
    {
        mPeers.emplace(userid, new Member(*this, userid, priv));
    });
}

void ChatRoom::syncRoomPropertiesWithApi(const mega::MegaTextChat &chat)
{
    if (chat.getShard() != mShardNo)
        throw std::runtime_error("syncWithApi: Shard number of chat can't change");
    if (chat.isGroup() != mIsGroup)
        throw std::runtime_error("syncWithApi: isGroup flag can't change");
    auto db = karere::gClient->db;
    auto url = chat.getUrl();
    if (!url)
        throw std::runtime_error("MegaTextChat::getUrl() returned NULL");
    if (strcmp(url, mUrl.c_str()))
    {
        mUrl = url;
        sqliteQuery(db, "update chats set url=? where chatid=?", mUrl, mChatid);
    }
    char ownPriv = chat.getOwnPrivilege();
    if (ownPriv != mOwnPriv)
    {
        mOwnPriv = ownPriv;
        sqliteQuery(db, "update chats set own_priv=? where chatid=?", ownPriv, mChatid);
    }
}
void ChatRoom::init(chatd::Messages* msgs, chatd::DbInterface*& dbIntf)
{
    mMessages = msgs;
    dbIntf = new ChatdSqliteDb(msgs, parent.client.db);
    if (mChatWindow)
    {
        switchListenerToChatWindow();
    }
}

IGui::IChatWindow &ChatRoom::chatWindow()
{
    if (!mChatWindow)
    {
        mChatWindow = parent.client.gui.createChatWindow(*this);
        mChatWindow->updateTitle(titleString());
        switchListenerToChatWindow();
    }
    return *mChatWindow;
}

void ChatRoom::switchListenerToChatWindow()
{
    if (mMessages->listener() == mChatWindow)
        return;
    chatd::DbInterface* dummyIntf = nullptr;
    mChatWindow->init(mMessages, dummyIntf);
    mMessages->setListener(*mChatWindow);
}

Presence PeerChatRoom::presence() const
{
    if (mMessages && mMessages->onlineState() != chatd::kChatStateOnline)
        return Presence::kOffline;
    return mContact->xmppContact().presence();
}

void ChatRoom::updateAllOnlineDisplays(Presence pres)
{
    if (mTitleDisplay)
        mTitleDisplay->updateOnlineIndication(pres);
    if (mChatWindow)
        mChatWindow->updateOnlineIndication(pres);
}

void GroupChatRoom::onUserJoined(const chatd::Id &userid, char privilege)
{
    addMember(userid, privilege, true);
}
void GroupChatRoom::onUserLeft(const chatd::Id &userid)
{
    removeMember(userid);
}

void PeerChatRoom::onUserJoined(const chatd::Id &userid, char privilege)
{
    if (userid == parent.client.chatd->userId())
        syncOwnPriv(privilege);
    else if (userid.val == mPeer)
        syncPeerPriv(privilege);
    else
        KR_LOG_ERROR("PeerChatRoom: Bug: Received JOIN event from chatd for a third user, ignoring");
}

void PeerChatRoom::onUserLeft(const chatd::Id &userid)
{
    KR_LOG_ERROR("PeerChatRoom: Bug: Received an user leave event from chatd on a permanent chat, ignoring");
}
void ChatRoom::onRecvNewMessage(chatd::Idx idx, chatd::Message &msg, chatd::Message::Status status)
{
    mTitleDisplay->updateOverlayCount(mMessages->unreadMsgCount());
}
void ChatRoom::onMessageStatusChange(chatd::Idx idx, chatd::Message::Status newStatus, const chatd::Message &msg)
{
    mTitleDisplay->updateOverlayCount(mMessages->unreadMsgCount());
}
void PeerChatRoom::onOnlineStateChange(chatd::ChatState state)
{
    if (state == chatd::kChatStateOnline)
    {
        mTitleDisplay->updateOverlayCount(mMessages->unreadMsgCount());
        updateAllOnlineDisplays(mContact->xmppContact().presence());
    }
    else
    {
        updateAllOnlineDisplays(Presence::kOffline);
    }
}
void GroupChatRoom::onOnlineStateChange(chatd::ChatState state)
{
    printf("group online status %d\n", state);
    updateAllOnlineDisplays((state == chatd::kChatStateOnline)
        ? Presence::kOnline
        : Presence::kOffline);
}

void GroupChatRoom::syncMembers(const chatd::UserPrivMap& users)
{
    auto db = karere::gClient->db;
    for (auto ourIt=mPeers.begin(); ourIt!=mPeers.end();)
    {
        auto userid = ourIt->first;
        auto it = users.find(userid);
        if (it == users.end()) //we have a user that is not in the chatroom anymore
        {
            auto erased = ourIt;
            ourIt++;
            auto member = erased->second;
            mPeers.erase(erased);
            delete member;
            sqliteQuery(db, "delete from chat_peers where chatid=? and userid=?", mChatid, userid);
        }
        else
        {
            if (ourIt->second->mPriv != it->second)
            {
                sqliteQuery(db, "update chat_peers where chatid=? and userid=? set priv=?",
                    mChatid, userid, it->second);
            }
            ourIt++;
        }
    }
    for (auto& user: users)
    {
        if (mPeers.find(user.first) == mPeers.end())
            addMember(user.first, user.second, true);
    }
}

void GroupChatRoom::syncWithApi(const mega::MegaTextChat& chat)
{
    ChatRoom::syncRoomPropertiesWithApi(chat);
    chatd::UserPrivMap membs;
    syncMembers(apiMembersToMap(chat, membs));
}


chatd::UserPrivMap& GroupChatRoom::apiMembersToMap(const mega::MegaTextChat& chat, chatd::UserPrivMap& membs)
{
    auto members = chat.getPeerList();
    if (!members)
        throw std::runtime_error("MegaTextChat::getPeers() returned NULL");

    auto size = members->size();
    for (int i=0; i<size; i++)
        membs.emplace(members->getPeerHandle(i), members->getPeerPrivilege(i));
    return membs;
}

GroupChatRoom::Member::Member(GroupChatRoom& aRoom, const uint64_t& user, char aPriv)
: mRoom(aRoom), mPriv(aPriv)
{
    mNameAttrCbHandle = mRoom.parent.client.userAttrCache.getAttr(user, mega::MegaApi::USER_ATTR_LASTNAME, this,
    [](Buffer* buf, void* userp)
    {
        auto self = static_cast<Member*>(userp);
        if (buf)
            self->mName.assign(buf->buf(), buf->dataSize());
        else if (self->mName.empty())
            self->mName = "\x07(error)";
        self->mRoom.updateTitle();
    });
}
GroupChatRoom::Member::~Member()
{
    mRoom.parent.client.userAttrCache.removeCb(mNameAttrCbHandle);
}

ContactList::ContactList(Client& aClient)
:client(aClient)
{
    SqliteStmt stmt(client.db, "select userid, email from contacts");
    while(stmt.step())
    {
        auto userid = stmt.uint64Col(0);
        emplace(userid, new Contact(*this, userid, stmt.stringCol(1), nullptr));
    }
}

bool ContactList::addUserFromApi(mega::MegaUser& user)
{
    auto userid = user.getHandle();
    auto& item = (*this)[userid];
    if (item)
        return false;
    auto cmail = user.getEmail();
    std::string email(cmail?cmail:"");

    sqliteQuery(client.db, "insert or replace into contacts(userid, email) values(?,?)", userid, email);
    item = new Contact(*this, userid, email, nullptr);
    KR_LOG_DEBUG("Added new user from API: %s", email.c_str());
    return true;
}

void ContactList::syncWithApi(mega::MegaUserList& users)
{
    std::set<uint64_t> apiUsers;
    auto size = users.size();
    auto me = client.myHandle();
    for (int i=0; i<size; i++)
    {
        auto& user = *users.get(i);
        if (user.getHandle() == me)
            continue;
        apiUsers.insert(user.getHandle());
        addUserFromApi(user);
    }
    for (auto it = begin(); it!= end();)
    {
        auto handle = it->first;
        if (apiUsers.find(handle) != apiUsers.end())
        {
            it++;
            continue;
        }
        auto erased = it;
        it++;
        removeUser(erased);
    }
}

void ContactList::removeUser(const uint64_t& userid)
{
    auto it = find(userid);
    if (it == end())
    {
        KR_LOG_ERROR("ContactList::removeUser: Unknown user");
        return;
    }
    removeUser(it);
}

void ContactList::removeUser(iterator it)
{
    auto handle = it->first;
    delete it->second;
    erase(it);
    sqliteQuery(client.db, "delete from contacts where userid=?", handle);
}

ContactList::~ContactList()
{
    for (auto& it: *this)
        delete it.second;
}

Contact::Contact(ContactList& clist, const uint64_t& userid,
                 const std::string& email, PeerChatRoom* room)
    :mClist(clist), mUserid(userid), mChatRoom(room), mEmail(email),
     mTitleString(email),
     mDisplay(clist.client.gui.contactList().createContactItem(*this))
{
    updateTitle(email);
    mUsernameAttrCbId = mClist.client.userAttrCache.getAttr(userid,
        mega::MegaApi::USER_ATTR_LASTNAME, this,
        [](Buffer* data, void* userp)
        {
            auto self = static_cast<Contact*>(userp);
            if (!data || data->dataSize() < 2)
                self->updateTitle(self->mEmail);
            else
                self->updateTitle(data->buf()+1);
        });
    mXmppContact = mClist.client.xmppContactList().addContact(*this);
}
void Contact::updateTitle(const std::string& str)
{
    mTitleString = str;
    mDisplay->updateTitle(str);
    if (mChatRoom && mChatRoom->hasChatWindow())
        mChatRoom->chatWindow().updateTitle(str);
}

Contact::~Contact()
{
    mClist.client.userAttrCache.removeCb(mUsernameAttrCbId);
    mClist.client.gui.contactList().removeContactItem(mDisplay);
}
promise::Promise<ChatRoom*> Contact::createChatRoom()
{
    if (mChatRoom)
    {
        KR_LOG_WARNING("Contact::createChatRoom: chat room already exists, check before caling this method");
        return Promise<ChatRoom*>(mChatRoom);
    }
    mega::MegaTextChatPeerListPrivate peers;
    peers.addPeer(mUserid, chatd::PRIV_FULL);
    return mClist.client.api->call(&mega::MegaApi::createChat, false, &peers)
    .then([this](ReqResult result) -> Promise<ChatRoom*>
    {
        auto list = *result->getMegaTextChatList();
        if (list.size() < 1)
            return promise::Error("Empty chat list returned from API");
        auto& room = mClist.client.chats->addRoom(*list.get(0));
        return &room;
    });
}

void Contact::setChatRoom(PeerChatRoom& room)
{
    assert(!mChatRoom);
    mChatRoom = &room;
    if (room.hasChatWindow())
        room.chatWindow().updateTitle(mTitleString);
}

IGui::ITitleDisplay*
ContactList::attachRoomToContact(const uint64_t& userid, PeerChatRoom& room)
{
    auto it = find(userid);
    if (it == end())
        throw std::runtime_error("attachRoomToContact: userid not found");
    auto& contact = *it->second;
    if (contact.mChatRoom)
        throw std::runtime_error("attachRoomToContact: contact already has a chat room attached");
    contact.setChatRoom(room);
    room.setContact(contact);
    return contact.mDisplay;
}
uint64_t Client::useridFromJid(const std::string& jid)
{
    auto end = jid.find('@');
    if (end != 13)
    {
        KR_LOG_WARNING("useridFromJid: Invalid Mega JID '%s'", jid.c_str());
        return mega::UNDEF;
    }

    uint64_t userid;
    auto len = mega::Base32::atob(jid.c_str(), (byte*)&userid, end);
    assert(len == 8);
    return userid;
}

Contact* ContactList::contactFromJid(const std::string& jid) const
{
    auto userid = Client::useridFromJid(jid);
    if (userid == mega::UNDEF)
        return nullptr;
    auto it = find(userid);
    if (it == this->end())
        return nullptr;
    else
        return it->second;
}

void Client::discoAddFeature(const char *feature)
{
    conn->plugin<disco::DiscoPlugin>("disco").addFeature(feature);
}
rtcModule::IEventHandler* Client::onIncomingCallRequest(
        const std::shared_ptr<rtcModule::ICallAnswer> &ans)
{
    return gui.createCallAnswerGui(ans);
}

}

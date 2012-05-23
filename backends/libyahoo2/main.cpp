// Transport includes
#include "transport/config.h"
#include "transport/networkplugin.h"
#include "transport/logging.h"

// Yahoo2
#include <yahoo2.h>
#include <yahoo2_callbacks.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

// Swiften
#include "Swiften/Swiften.h"
#include "Swiften/TLS/OpenSSL/OpenSSLContextFactory.h"

// for signal handler
#include "unistd.h"
#include "signal.h"
#include "sys/wait.h"
#include "sys/signal.h"

// Boost
#include <boost/algorithm/string.hpp>
using namespace boost::filesystem;
using namespace boost::program_options;
using namespace Transport;


typedef struct {
	int handler_tag;
	int conn_tag;
	void *data;
	yahoo_input_condition cond;
} yahoo_handler;

typedef struct {
	int id;
	std::map<int, boost::shared_ptr<Swift::Connection> > conns;
	int conn_tag;
	std::map<int, yahoo_handler *> handlers;
	std::map<int, std::map<int, yahoo_handler *> > handlers_per_conn;
	int handler_tag;
	int status;
	std::string msg;
	std::string buffer;
} yahoo_local_account;

static std::string *currently_read_data;
static yahoo_local_account *currently_writting_account;

typedef struct {
	std::string yahoo_id;
	std::string name;
	int status;
	int away;
	std::string msg;
	std::string group;
} yahoo_account;

typedef struct {
	int id;
	char *label;
} yahoo_idlabel;

typedef struct {
	int id;
	char *who;
} yahoo_authorize_data;

DEFINE_LOGGER(logger, "Yahoo2");

// eventloop
Swift::SimpleEventLoop *loop_;

// Plugin
class YahooPlugin;
YahooPlugin * np = NULL;

class YahooPlugin : public NetworkPlugin {
	public:
		Swift::BoostNetworkFactories *m_factories;
		Swift::OpenSSLContextFactory *m_sslFactory;
		Swift::TLSConnectionFactory *m_tlsFactory;
		Swift::BoostIOServiceThread m_boostIOServiceThread;
		boost::shared_ptr<Swift::Connection> m_conn;

		YahooPlugin(Config *config, Swift::SimpleEventLoop *loop, const std::string &host, int port) : NetworkPlugin() {
			this->config = config;
			m_factories = new Swift::BoostNetworkFactories(loop);
			m_sslFactory = new Swift::OpenSSLContextFactory();
			m_tlsFactory = new Swift::TLSConnectionFactory(m_sslFactory, m_factories->getConnectionFactory());
			m_conn = m_factories->getConnectionFactory()->createConnection();
			m_conn->onDataRead.connect(boost::bind(&YahooPlugin::_handleDataRead, this, _1));
			m_conn->connect(Swift::HostAddressPort(Swift::HostAddress(host), port));

			LOG4CXX_INFO(logger, "Starting the plugin.");
		}

		// NetworkPlugin uses this method to send the data to networkplugin server
		void sendData(const std::string &string) {
			m_conn->write(Swift::createSafeByteArray(string));
		}

		// This method has to call handleDataRead with all received data from network plugin server
		void _handleDataRead(boost::shared_ptr<Swift::SafeByteArray> data) {
			std::string d(data->begin(), data->end());
			handleDataRead(d);
		}

		void handleLoginRequest(const std::string &user, const std::string &legacyName, const std::string &password) {
			yahoo_local_account *account = new yahoo_local_account;
			account->conn_tag = 1;
			account->handler_tag = 1;
			m_users[user] = account;

			account->id = yahoo_init_with_attributes(legacyName.c_str(), password.c_str(), 
					"local_host", "",
					"pager_port", 5050,
					NULL);
			m_ids[account->id] = user;

			account->status = YAHOO_STATUS_OFFLINE;
			LOG4CXX_INFO(logger, user << ": Logging in the user as " << legacyName << " with id=" << account->id);
			yahoo_login(account->id, YAHOO_STATUS_AVAILABLE);
		}

		void handleLogoutRequest(const std::string &user, const std::string &legacyName) {
		}

		void handleMessageSendRequest(const std::string &user, const std::string &legacyName, const std::string &message, const std::string &xhtml = "") {
			LOG4CXX_INFO(logger, "Sending message from " << user << " to " << legacyName << ".");
			if (legacyName == "echo") {
				handleMessage(user, legacyName, message);
			}
		}

		void handleBuddyUpdatedRequest(const std::string &user, const std::string &buddyName, const std::string &alias, const std::vector<std::string> &groups) {
			LOG4CXX_INFO(logger, user << ": Added buddy " << buddyName << ".");
			handleBuddyChanged(user, buddyName, alias, groups, pbnetwork::STATUS_ONLINE);
		}

		void handleBuddyRemovedRequest(const std::string &user, const std::string &buddyName, const std::vector<std::string> &groups) {

		}

		yahoo_local_account *getAccount(int id) {
			return m_users[m_ids[id]];
		}

		void _yahoo_connect_finished(yahoo_local_account *account, yahoo_connect_callback callback, void *data, int conn_tag, bool error) {
			if (error) {
				LOG4CXX_ERROR(logger, "Connection error!");
				callback(NULL, 0, data);
// 				np->handleDisconnected(user, 0, "Connection error.");
			}
			else {
				LOG4CXX_INFO(logger, "Connected");
				// We will have dangling pointer here, but we can't pass boost::shared_ptr here...
				callback((void *) conn_tag, 0, data);
			}
		}

		void _yahoo_data_read(yahoo_local_account *account, int conn_tag, boost::shared_ptr<Swift::SafeByteArray> data) {
			// yahoo_read_ready calls ext_yahoo_read(...) in a loop, so we just have to choose proper buffer from which will
			// that method read. We do that by static currently_read_data pointer.
			std::string d(data->begin(), data->end());

			LOG4CXX_INFO(logger, "data to read");
			for (std::map<int, yahoo_handler *>::iterator it = account->handlers_per_conn[conn_tag].begin(); it != account->handlers_per_conn[conn_tag].end(); it++) {
				if (it->second->cond == YAHOO_INPUT_READ) {
					LOG4CXX_INFO(logger, "found handler");
					std::string cpy(d);
					currently_read_data = &cpy;
					yahoo_read_ready(account->id, (void *) conn_tag, it->second->data);
				}
			}
		}

		void _yahoo_data_written(yahoo_local_account *account, int conn_tag) {
			LOG4CXX_INFO(logger, "data written");
			currently_writting_account = account;
			for (std::map<int, yahoo_handler *>::iterator it = account->handlers_per_conn[conn_tag].begin(); it != account->handlers_per_conn[conn_tag].end(); it++) {
				if (it->second->cond == YAHOO_INPUT_WRITE) {
					yahoo_write_ready(account->id, (void *) conn_tag, it->second->data);
				}
			}
		}

		int _yahoo_connect_async(int id, const char *host, int port, yahoo_connect_callback callback, void *data, int use_ssl) {
			yahoo_local_account *account = getAccount(id);
			if (!account) {
				LOG4CXX_ERROR(logger, "Unknown account id=" << id);
				return -1;
			}

// boost::asio::io_service io_service;
// boost::asio::ip::tcp::resolver resolver(io_service);
// boost::asio::ip::tcp::resolver::query query(values[1], "");
// for(boost::asio::ip::tcp::resolver::iterator i = resolver.resolve(query);
//                             i != boost::asio::ip::tcp::resolver::iterator();
//                             ++i)
// {
//     boost::asio::ip::tcp::endpoint end = *i;
//     std::cout << end.address() << ' ';
// }
// std::cout << '\n';


			LOG4CXX_INFO(logger, m_ids[id] << ": Connecting " << host << ":" << port);
			int tag = account->conn_tag++;
			if (use_ssl) {
				account->conns[tag] = m_tlsFactory->createConnection();
			}
			else {
				account->conns[tag] = m_factories->getConnectionFactory()->createConnection();
			}
			account->conns[tag]->onConnectFinished.connect(boost::bind(&YahooPlugin::_yahoo_connect_finished, this, account, callback, data, tag, _1));
			account->conns[tag]->onDataRead.connect(boost::bind(&YahooPlugin::_yahoo_data_read, this, account, tag, _1));
			account->conns[tag]->onDataWritten.connect(boost::bind(&YahooPlugin::_yahoo_data_written, this, account, tag));
			account->conns[tag]->connect(Swift::HostAddressPort(Swift::HostAddress("67.195.187.249"), port));
			return tag;
		}

	private:
		Config *config;
		std::map<std::string, yahoo_local_account *> m_users;
		std::map<int, std::string> m_ids;
};

static void spectrum_sigchld_handler(int sig)
{
	int status;
	pid_t pid;

	do {
		pid = waitpid(-1, &status, WNOHANG);
	} while (pid != 0 && pid != (pid_t)-1);

	if ((pid == (pid_t) - 1) && (errno != ECHILD)) {
		char errmsg[BUFSIZ];
		snprintf(errmsg, BUFSIZ, "Warning: waitpid() returned %d", pid);
		perror(errmsg);
	}
}

static void ext_yahoo_got_conf_invite(int id, const char *me, const char *who, const char *room, const char *msg, YList *members) {
}

static void ext_yahoo_conf_userdecline(int id, const char *me, const char *who, const char *room, const char *msg) {
}

static void ext_yahoo_conf_userjoin(int id, const char *me, const char *who, const char *room) {
}

static void ext_yahoo_conf_userleave(int id, const char *me, const char *who, const char *room) {
}

static void ext_yahoo_conf_message(int id, const char *me, const char *who, const char *room, const char *msg, int utf8) {
}

static void ext_yahoo_chat_cat_xml(int id, const char *xml)  {
}

static void ext_yahoo_chat_join(int id, const char *me, const char *room, const char * topic, YList *members, void *fd) {
}

static void ext_yahoo_chat_userjoin(int id, const char *me, const char *room, struct yahoo_chat_member *who) {
}

static void ext_yahoo_chat_userleave(int id, const char *me, const char *room, const char *who) {
}

static void ext_yahoo_chat_message(int id, const char *me, const char *who, const char *room, const char *msg, int msgtype, int utf8) {
}

static void ext_yahoo_status_changed(int id, const char *who, int stat, const char *msg, int away, int idle, int mobile) {
}

static void ext_yahoo_got_buddies(int id, YList * buds) {
// 	while(buddies) {
// 		FREE(buddies->data);
// 		buddies = buddies->next;
// 		if(buddies)
// 			FREE(buddies->prev);
// 	}
// 	for(; buds; buds = buds->next) {
// 		yahoo_account *ya = y_new0(yahoo_account, 1);
// 		struct yahoo_buddy *bud = buds->data;
// 		strncpy(ya->yahoo_id, bud->id, 255);
// 		if(bud->real_name)
// 			strncpy(ya->name, bud->real_name, 255);
// 		strncpy(ya->group, bud->group, 255);
// 		ya->status = YAHOO_STATUS_OFFLINE;
// 		buddies = y_list_append(buddies, ya);
// 
// /*		print_message(("%s is %s", bud->id, bud->real_name));*/
// 	}
}

static void ext_yahoo_got_ignore(int id, YList * igns)
{
}

static void ext_yahoo_got_buzz(int id, const char *me, const char *who, long tm) {
}

static void ext_yahoo_got_im(int id, const char *me, const char *who, const char *msg, long tm, int stat, int utf8) {
}

static void ext_yahoo_rejected(int id, const char *who, const char *msg) {
}

static void ext_yahoo_contact_added(int id, const char *myid, const char *who, const char *msg) {
}

static void ext_yahoo_typing_notify(int id, const char* me, const char *who, int stat) {
}

static void ext_yahoo_game_notify(int id, const char *me, const char *who, int stat, const char *msg)
{
}

static void ext_yahoo_mail_notify(int id, const char *from, const char *subj, int cnt) {
}

static void ext_yahoo_got_webcam_image(int id, const char *who, const unsigned char *image, unsigned int image_size, unsigned int real_size, unsigned int timestamp) {
}

static void ext_yahoo_webcam_viewer(int id, const char *who, int connect) {
}

static void ext_yahoo_webcam_closed(int id, const char *who, int reason) {
}

static void ext_yahoo_webcam_data_request(int id, int send) {
}

static void ext_yahoo_webcam_invite(int id, const char *me, const char *from) {
}

static void ext_yahoo_webcam_invite_reply(int id, const char *me, const char *from, int accept) {
}

static void ext_yahoo_system_message(int id, const char *me, const char *who, const char *msg) {
}

static void ext_yahoo_got_cookies(int id) {
}

static void ext_yahoo_login_response(int id, int succ, const char *url) {
	LOG4CXX_INFO(logger, "login_response");
// 	char buff[1024];
// 
// 	if(succ == YAHOO_LOGIN_OK) {
// 		ylad->status = yahoo_current_status(id);
// 		print_message(("logged in"));
// 		return;
// 		
// 	} else if(succ == YAHOO_LOGIN_UNAME) {
// 
// 		snprintf(buff, sizeof(buff), "Could not log into Yahoo service - username not recognised.  Please verify that your username is correctly typed.");
// 	} else if(succ == YAHOO_LOGIN_PASSWD) {
// 
// 		snprintf(buff, sizeof(buff), "Could not log into Yahoo service - password incorrect.  Please verify that your password is correctly typed.");
// 
// 	} else if(succ == YAHOO_LOGIN_LOCK) {
// 		
// 		snprintf(buff, sizeof(buff), "Could not log into Yahoo service.  Your account has been locked.\nVisit %s to reactivate it.", url);
// 
// 	} else if(succ == YAHOO_LOGIN_DUPL) {
// 
// 		snprintf(buff, sizeof(buff), "You have been logged out of the yahoo service, possibly due to a duplicate login.");
// 	} else if(succ == YAHOO_LOGIN_SOCK) {
// 
// 		snprintf(buff, sizeof(buff), "The server closed the socket.");
// 	} else {
// 		snprintf(buff, sizeof(buff), "Could not log in, unknown reason: %d.", succ);
// 	}
// 
// 	ylad->status = YAHOO_STATUS_OFFLINE;
// 	print_message((buff));
// 	yahoo_logout();
// 	poll_loop=0;
}

static void ext_yahoo_error(int id, const char *_err, int fatal, int num) {
	std::string err(_err);
	std::string msg("Yahoo Error: ");
	msg += err + " - ";
	switch(num) {
		case E_UNKNOWN:
			msg += "unknown error " + err;
			break;
		case E_CUSTOM:
			msg += "custom error " + err;
			break;
		case E_CONFNOTAVAIL:
			msg += err + " is not available for the conference";
			break;
		case E_IGNOREDUP:
			msg += err + " is already ignored";
			break;
		case E_IGNORENONE:
			msg += err +" is not in the ignore list";
			break;
		case E_IGNORECONF:
			msg += err + " is in buddy list - cannot ignore ";
			break;
		case E_SYSTEM:
			msg += "system error " + err;
			break;
		case E_CONNECTION:
			msg += err + "server connection error %s";
			break;
	}
	LOG4CXX_ERROR(logger, msg);
// 	if(fatal)
// 		yahoo_logout();
}

static int ext_yahoo_connect(const char *host, int port) {
	return -1;
}

static int ext_yahoo_add_handler(int id, void *fd, yahoo_input_condition cond, void *data) {
	yahoo_local_account *account = np->getAccount(id);
	if (!account) {
		return -1;
	}

	LOG4CXX_INFO(logger, "Adding handler " << cond);

	int conn_tag = (unsigned long) fd;
	yahoo_handler *handler = new yahoo_handler;

	handler->conn_tag = conn_tag;
	handler->handler_tag = account->handler_tag++;
	handler->data = data;
	handler->cond = cond;

	if (cond == YAHOO_INPUT_WRITE) {
		yahoo_local_account *old = currently_writting_account;
		currently_writting_account = account;
		yahoo_write_ready(id, fd, data);
		currently_writting_account = old;
	}

	account->handlers[handler->handler_tag] = handler;
	account->handlers_per_conn[conn_tag][handler->handler_tag] = handler;

	return handler->handler_tag;
}

static void ext_yahoo_remove_handler(int id, int tag) {
	yahoo_local_account *account = np->getAccount(id);
	if (!account) {
		return;
	}

	if (account->handlers.find(tag) == account->handlers.end()) {
		return;
	}

	yahoo_handler *handler = account->handlers[tag];
	account->handlers.erase(tag);
	account->handlers_per_conn[handler->conn_tag].erase(tag);
	delete handler;
}

static int ext_yahoo_write(void *fd, char *buf, int len) {
	LOG4CXX_INFO(logger, "Writting " << len);

	int conn_tag = (unsigned long) fd;

	yahoo_local_account *account = currently_writting_account;

	std::string string(buf, len);
	account->conns[conn_tag]->write(Swift::createSafeByteArray(string));

	return len;
}

static int ext_yahoo_read(void *fd, char *buf, int len) {
	if (currently_read_data->size() < len) {
		len = currently_read_data->size();
	}
	LOG4CXX_INFO(logger, "Reading " << len);
	memcpy(buf, currently_read_data->c_str(), len);
	currently_read_data->erase(0, len);
	return len;
}

static void ext_yahoo_close(void *fd) {
}

static int ext_yahoo_connect_async(int id, const char *host, int port, yahoo_connect_callback callback, void *data, int use_ssl) {
	return np->_yahoo_connect_async(id, host, port, callback, data, use_ssl);
}

static void ext_yahoo_got_file(int id, const char *me, const char *who, const char *msg, const char *fname, unsigned long fesize, char *trid) {
}

static void ext_yahoo_file_transfer_done(int id, int response, void *data) {
}

static char *ext_yahoo_get_ip_addr(const char *domain) {
	return NULL;
}

static void ext_yahoo_got_ft_data(int id, const unsigned char *in, int count, void *data) {
}

static void ext_yahoo_got_identities(int id, YList * ids) {
}

static void ext_yahoo_chat_yahoologout(int id, const char *me) {
}

static void ext_yahoo_chat_yahooerror(int id, const char *me) {
}

static void ext_yahoo_got_ping(int id, const char *errormsg){
}

static void ext_yahoo_got_search_result(int id, int found, int start, int total, YList *contacts) {
}

static void ext_yahoo_got_buddyicon_checksum(int id, const char *a, const char *b, int checksum) {
}

static void ext_yahoo_got_buddy_change_group(int id, const char *me, const char *who, const char *old_group, const char *new_group) {
}

static void ext_yahoo_got_buddyicon(int id, const char *a, const char *b, const char *c, int checksum) {
}

static void ext_yahoo_buddyicon_uploaded(int id, const char *url) {
}

static void ext_yahoo_got_buddyicon_request(int id, const char *me, const char *who) {
}

static int ext_yahoo_log(const char *fmt,...)
{
	static char log[8192];
	va_list ap;

	va_start(ap, fmt);

	vsnprintf(log, 8191, fmt, ap);
	LOG4CXX_INFO(logger, log);
	fflush(stderr);
	va_end(ap);
	return 0;
}

static void register_callbacks()
{
	static struct yahoo_callbacks yc;

	yc.ext_yahoo_login_response = ext_yahoo_login_response;
	yc.ext_yahoo_got_buddies = ext_yahoo_got_buddies;
	yc.ext_yahoo_got_ignore = ext_yahoo_got_ignore;
	yc.ext_yahoo_got_identities = ext_yahoo_got_identities;
	yc.ext_yahoo_got_cookies = ext_yahoo_got_cookies;
	yc.ext_yahoo_status_changed = ext_yahoo_status_changed;
	yc.ext_yahoo_got_im = ext_yahoo_got_im;
	yc.ext_yahoo_got_buzz = ext_yahoo_got_buzz;
	yc.ext_yahoo_got_conf_invite = ext_yahoo_got_conf_invite;
	yc.ext_yahoo_conf_userdecline = ext_yahoo_conf_userdecline;
	yc.ext_yahoo_conf_userjoin = ext_yahoo_conf_userjoin;
	yc.ext_yahoo_conf_userleave = ext_yahoo_conf_userleave;
	yc.ext_yahoo_conf_message = ext_yahoo_conf_message;
	yc.ext_yahoo_chat_cat_xml = ext_yahoo_chat_cat_xml;
	yc.ext_yahoo_chat_join = ext_yahoo_chat_join;
	yc.ext_yahoo_chat_userjoin = ext_yahoo_chat_userjoin;
	yc.ext_yahoo_chat_userleave = ext_yahoo_chat_userleave;
	yc.ext_yahoo_chat_message = ext_yahoo_chat_message;
	yc.ext_yahoo_chat_yahoologout = ext_yahoo_chat_yahoologout;
	yc.ext_yahoo_chat_yahooerror = ext_yahoo_chat_yahooerror;
	yc.ext_yahoo_got_webcam_image = ext_yahoo_got_webcam_image;
	yc.ext_yahoo_webcam_invite = ext_yahoo_webcam_invite;
	yc.ext_yahoo_webcam_invite_reply = ext_yahoo_webcam_invite_reply;
	yc.ext_yahoo_webcam_closed = ext_yahoo_webcam_closed;
	yc.ext_yahoo_webcam_viewer = ext_yahoo_webcam_viewer;
	yc.ext_yahoo_webcam_data_request = ext_yahoo_webcam_data_request;
	yc.ext_yahoo_got_file = ext_yahoo_got_file;
	yc.ext_yahoo_got_ft_data = ext_yahoo_got_ft_data;
	yc.ext_yahoo_get_ip_addr = ext_yahoo_get_ip_addr;
	yc.ext_yahoo_file_transfer_done = ext_yahoo_file_transfer_done;
	yc.ext_yahoo_contact_added = ext_yahoo_contact_added;
	yc.ext_yahoo_rejected = ext_yahoo_rejected;
	yc.ext_yahoo_typing_notify = ext_yahoo_typing_notify;
	yc.ext_yahoo_game_notify = ext_yahoo_game_notify;
	yc.ext_yahoo_mail_notify = ext_yahoo_mail_notify;
	yc.ext_yahoo_got_search_result = ext_yahoo_got_search_result;
	yc.ext_yahoo_system_message = ext_yahoo_system_message;
	yc.ext_yahoo_error = ext_yahoo_error;
	yc.ext_yahoo_log = ext_yahoo_log;
	yc.ext_yahoo_add_handler = ext_yahoo_add_handler;
	yc.ext_yahoo_remove_handler = ext_yahoo_remove_handler;
	yc.ext_yahoo_connect = ext_yahoo_connect;
	yc.ext_yahoo_connect_async = ext_yahoo_connect_async;
	yc.ext_yahoo_read = ext_yahoo_read;
	yc.ext_yahoo_write = ext_yahoo_write;
	yc.ext_yahoo_close = ext_yahoo_close;
	yc.ext_yahoo_got_buddyicon = ext_yahoo_got_buddyicon;
	yc.ext_yahoo_got_buddyicon_checksum = ext_yahoo_got_buddyicon_checksum;
	yc.ext_yahoo_buddyicon_uploaded = ext_yahoo_buddyicon_uploaded;
	yc.ext_yahoo_got_buddyicon_request = ext_yahoo_got_buddyicon_request;
	yc.ext_yahoo_got_ping = ext_yahoo_got_ping;
	yc.ext_yahoo_got_buddy_change_group = ext_yahoo_got_buddy_change_group;

	yahoo_register_callbacks(&yc);
}

int main (int argc, char* argv[]) {
	std::string host;
	int port;

	if (signal(SIGCHLD, spectrum_sigchld_handler) == SIG_ERR) {
		std::cout << "SIGCHLD handler can't be set\n";
		return -1;
	}

	boost::program_options::options_description desc("Usage: spectrum [OPTIONS] <config_file.cfg>\nAllowed options");
	desc.add_options()
		("host,h", value<std::string>(&host), "host")
		("port,p", value<int>(&port), "port")
		;
	try
	{
		boost::program_options::variables_map vm;
		boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
		boost::program_options::notify(vm);
	}
	catch (std::runtime_error& e)
	{
		std::cout << desc << "\n";
		exit(1);
	}
	catch (...)
	{
		std::cout << desc << "\n";
		exit(1);
	}


	if (argc < 5) {
		return 1;
	}

	Config config;
	if (!config.load(argv[5])) {
		std::cerr << "Can't open " << argv[1] << " configuration file.\n";
		return 1;
	}

	Swift::logging=true;
	Logging::initBackendLogging(&config);

	register_callbacks();
	yahoo_set_log_level(YAHOO_LOG_DEBUG);

	Swift::SimpleEventLoop eventLoop;
	loop_ = &eventLoop;
	np = new YahooPlugin(&config, &eventLoop, host, port);
	loop_->run();

	return 0;
}
#pragma once

#include <list>

#include "sip/sip_parser.h"
#include "AmThread.h"

#include "cdr/Cdr.h"
#include "SqlCallProfile.h"
#include "resources/Resource.h"

class SqlRouter;

class fake_logger: public msg_logger {
    sip_msg msg;
    int code;
  public:
    int log(const char* buf, int len,
            sockaddr_storage* src_ip,
            sockaddr_storage* dst_ip,
            cstring method, int reply_code=0);
    int relog(msg_logger *logger);
};

struct CallCtx
{
	//instead of atomic_int. guarded by SBCCallLeg::call_ctx_mutex
	unsigned int references;

	std::unique_ptr<Cdr> cdr;
	list<SqlCallProfile> profiles;
	list<SqlCallProfile>::iterator current_profile;
	AmSipRequest *initial_invite;
	vector<SdpMedia> aleg_negotiated_media;
	vector<SdpMedia> bleg_negotiated_media;
	bool SQLexception;
	bool on_hold;
	bool bleg_early_media_muted;
	bool ringing_timeout;
	bool ringing_sent;

	string referrer_session;
	bool transfer_intermediate_state;

	AmSdp bleg_initial_offer;

	SqlRouter &router;

	CallCtx(SqlRouter &router);
	~CallCtx();

	SqlCallProfile *getFirstProfile();
	SqlCallProfile *getNextProfile(bool early_state, bool resource_failover = false);
	SqlCallProfile *getCurrentProfile();

	void setRingingTimeout() { ringing_timeout = true; }
	bool isRingingTimeout() { return ringing_timeout; }

	vector<SdpMedia> &get_self_negotiated_media(bool a_leg);
	vector<SdpMedia> &get_other_negotiated_media(bool a_leg);

	ResourceList &getCurrentResourceList();
	int getOverrideId(bool aleg = true);
};

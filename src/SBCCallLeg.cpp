#include "SBCCallLeg.h"

#include "SBCCallControlAPI.h"

#include "log.h"
#include "AmUtils.h"
#include "AmAudio.h"
#include "AmPlugIn.h"
#include "AmMediaProcessor.h"
#include "AmConfigReader.h"
#include "AmSessionContainer.h"
#include "AmSipHeaders.h"
#include "SBCSimpleRelay.h"
#include "RegisterDialog.h"
#include "SubscriptionDialog.h"

#include "sip/pcap_logger.h"
#include "sip/sip_parser.h"
#include "sip/sip_trans.h"

#include "HeaderFilter.h"
#include "ParamReplacer.h"
#include "SDPFilter.h"

#include <algorithm>

#include "AmAudioFileRecorder.h"
#include "radius_hooks.h"
#include "Sensors.h"

#include "sdp_filter.h"
#include "ampi/RadiusClientAPI.h"
#include "dtmf_sip_info.h"

using namespace std;

#define TRACE DBG

#define FILE_RECORDER_COMPRESSED_EXT ".mp3"
#define FILE_RECORDER_RAW_EXT        ".wav"

inline void replace(string& s, const string& from, const string& to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos) {
        s.replace(pos, from.length(), to);
        pos += s.length();
    }
}

static const char *callStatus2str(const CallLeg::CallStatus state)
{
    static const char *disconnected = "Disconnected";
    static const char *disconnecting = "Disconnecting";
    static const char *noreply = "NoReply";
    static const char *ringing = "Ringing";
    static const char *connected = "Connected";
    static const char *unknown = "???";

    switch (state) {
        case CallLeg::Disconnected: return disconnected;
        case CallLeg::Disconnecting: return disconnecting;
        case CallLeg::NoReply: return noreply;
        case CallLeg::Ringing: return ringing;
        case CallLeg::Connected: return connected;
    }

    return unknown;
}

#define getCtx_void \
    if(NULL==call_ctx) {\
        ERROR("CallCtx = nullptr ");\
        log_stacktrace(L_ERR);\
        return;\
    }

#define getCtx_chained \
    if(NULL==call_ctx) {\
        ERROR("CallCtx = nullptr ");\
        log_stacktrace(L_ERR);\
        break;\
    }

#define with_cdr_for_read \
    Cdr *cdr = call_ctx->getCdrSafe<false>();\
    if(cdr)

#define with_cdr_for_write \
    Cdr *cdr = call_ctx->getCdrSafe<true>();\
    if(cdr)

///////////////////////////////////////////////////////////////////////////////////////////

// map stream index and transcoder payload index (two dimensions) into one under
// presumption that there will be less than 128 payloads for transcoding
// (might be handy to remember mapping only for dynamic ones (96-127)
#define MAP_INDEXES(stream_idx, payload_idx) ((stream_idx) * 128 + payload_idx)

void PayloadIdMapping::map(int stream_index, int payload_index, int payload_id)
{
    mapping[MAP_INDEXES(stream_index, payload_index)] = payload_id;
}

int PayloadIdMapping::get(int stream_index, int payload_index)
{
    std::map<int, int>::iterator i = mapping.find(MAP_INDEXES(stream_index, payload_index));
    if (i != mapping.end()) return i->second;
    else return -1;
}

void PayloadIdMapping::reset()
{
    mapping.clear();
}

#undef MAP_INDEXES

///////////////////////////////////////////////////////////////////////////////////////////

// A leg constructor (from SBCDialog)
SBCCallLeg::SBCCallLeg(
    CallCtx *call_ctx,
    AmSipDialog* p_dlg,
    AmSipSubscription* p_subs)
  : CallLeg(p_dlg,p_subs),
    m_state(BB_Init),
    auth(NULL),
    logger(NULL),
    sensor(NULL),
    yeti(Yeti::instance()),
    call_ctx(call_ctx),
    router(yeti.router),
    cdr_list(yeti.cdr_list),
    rctl(yeti.rctl),
    call_profile(*call_ctx->getCurrentProfile()),
    placeholders_hash(call_profile.placeholders_hash)
{
    set_sip_relay_only(false);
    dlg->setRel100State(Am100rel::REL100_IGNORED);

    if(call_profile.rtprelay_bw_limit_rate > 0
       && call_profile.rtprelay_bw_limit_peak > 0)
    {
        RateLimit* limit = new RateLimit(
            call_profile.rtprelay_bw_limit_rate,
            call_profile.rtprelay_bw_limit_peak,
            1000);
        rtp_relay_rate_limit.reset(limit);
    }

    if(call_profile.global_tag.empty()) {
        global_tag = getLocalTag();
    } else {
        global_tag = call_profile.global_tag;
    }
}

// B leg constructor (from SBCCalleeSession)
SBCCallLeg::SBCCallLeg(
    SBCCallLeg* caller,
    AmSipDialog* p_dlg,
    AmSipSubscription* p_subs)
  : auth(NULL),
    call_profile(caller->getCallProfile()),
    placeholders_hash(caller->getPlaceholders()),
    CallLeg(caller,p_dlg,p_subs),
    global_tag(caller->getGlobalTag()),
    logger(NULL),
    sensor(NULL),
    call_ctx(caller->getCallCtx()),
    yeti(Yeti::instance()),
    router(yeti.router),
    cdr_list(yeti.cdr_list),
    rctl(yeti.rctl)
{
  dlg->setRel100State(Am100rel::REL100_IGNORED);

    // we need to apply it here instead of in applyBProfile because we have caller
    // here (FIXME: do it on better place and better way than accessing internals
    // of caller->dlg directly)
    if (call_profile.transparent_dlg_id && caller) {
        dlg->setCallid(caller->dlg->getCallid());
        dlg->setExtLocalTag(caller->dlg->getRemoteTag());
        dlg->cseq = caller->dlg->r_cseq;
    }

    // copy RTP rate limit from caller leg
    if(caller->rtp_relay_rate_limit.get()) {
        rtp_relay_rate_limit.reset(new RateLimit(*caller->rtp_relay_rate_limit.get()));
    }

    init();

    setLogger(caller->getLogger());
}

SBCCallLeg::SBCCallLeg(AmSipDialog* p_dlg, AmSipSubscription* p_subs)
  : CallLeg(p_dlg,p_subs),
    m_state(BB_Init),
    auth(NULL),
    logger(NULL),
    sensor(NULL),
    yeti(Yeti::instance()),
    router(yeti.router),
    cdr_list(yeti.cdr_list),
    rctl(yeti.rctl)
{ }

void SBCCallLeg::init()
{
    call_ctx->inc();

    Cdr *cdr = call_ctx->cdr;

    if(a_leg) {
        ostringstream ss;
        ss << yeti.config.msg_logger_dir << '/' <<
              getLocalTag() << "_" <<
              int2str(yeti.config.node_id) << ".pcap";
        call_profile.set_logger_path(ss.str());

        cdr->update_sbc(call_profile);
        setSensor(Sensors::instance()->getSensor(call_profile.aleg_sensor_id));
        cdr->update_init_aleg(getLocalTag(),
                              global_tag,
                              getCallID());
    } else {
        if(!call_profile.callid.empty()){
            string id = AmSession::getNewId();
            replace(call_profile.callid,"%uuid",id);
        }
        setSensor(Sensors::instance()->getSensor(call_profile.bleg_sensor_id));
        cdr->update_init_bleg(call_profile.callid.empty()? getCallID() : call_profile.callid);
    }

    if(call_profile.record_audio){
        ostringstream ss;
        ss  << yeti.config.audio_recorder_dir << '/'
            << global_tag << "_"
            << int2str(yeti.config.node_id) <<  "_leg"
            << (a_leg ? "a" : "b")
            << (yeti.config.audio_recorder_compress ?
                FILE_RECORDER_COMPRESSED_EXT :
                FILE_RECORDER_RAW_EXT);
        call_profile.audio_record_path = ss.str();

        AmAudioFileRecorderProcessor::instance()->addRecorder(
            getLocalTag(),
            call_profile.audio_record_path);
        setRecordAudio(true);
    }
}

void SBCCallLeg::terminateLegOnReplyException(const AmSipReply& reply,const InternalException &e)
{
    getCtx_void

    if(!a_leg) {
        if(!getOtherId().empty()) { //ignore not connected B legs
            with_cdr_for_read {
                cdr->update_internal_reason(DisconnectByTS,e.internal_reason,e.internal_code);
                cdr->update(reply);
            }
        }
        relayError(reply.cseq_method,reply.cseq,true,e.response_code,e.response_reason.c_str());
        disconnect(false,false);
    } else {
        with_cdr_for_read {
            cdr->update_internal_reason(DisconnectByTS,e.internal_reason,e.internal_code);
            cdr->update(reply);
        }
    }
    stopCall(CallLeg::StatusChangeCause::InternalError);
}

void SBCCallLeg::processRouting()
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    SqlCallProfile *profile = NULL;
/*    AmSipRequest &req = aleg_modified_req;
    AmSipRequest &b_req = modified_req;*/

    ResourceCtlResponse rctl_ret;
    ResourceList::iterator ri;
    string refuse_reason;
    int refuse_code;
    int attempt = 0;

    Cdr *cdr = call_ctx->cdr;

    PROF_START(func);

    try {

    PROF_START(rchk);
    do {
        DBG("%s() check resources for profile. attempt %d",FUNC_NAME,attempt);
        rctl_ret = rctl.get(call_ctx->getCurrentResourceList(),
                            call_ctx->getCurrentProfile()->resource_handler,
                            getLocalTag(),
                            refuse_code,refuse_reason,ri);

        if(rctl_ret == RES_CTL_OK){
            DBG("%s() check resources succ",FUNC_NAME);
            break;
        } else if(	rctl_ret ==  RES_CTL_REJECT ||
                    rctl_ret ==  RES_CTL_ERROR){
            DBG("%s() check resources failed with code: %d, reply: <%d '%s'>",FUNC_NAME,
                rctl_ret,refuse_code,refuse_reason.c_str());
            if(rctl_ret == RES_CTL_REJECT) {
                cdr->update_failed_resource(*ri);
            }
            break;
        } else if(	rctl_ret == RES_CTL_NEXT){
            DBG("%s() check resources failed with code: %d, reply: <%d '%s'>",FUNC_NAME,
                rctl_ret,refuse_code,refuse_reason.c_str());

            profile = call_ctx->getNextProfile(true);

            if(NULL==profile){
                cdr->update_failed_resource(*ri);
                DBG("%s() there are no profiles more",FUNC_NAME);
                throw AmSession::Exception(503,"no more profiles");
            }

            DBG("%s() choosed next profile",FUNC_NAME);

            /* show resource disconnect reason instead of
             * refuse_profile if refuse_profile follows failed resource with
             * failover to next */
            if(profile->disconnect_code_id!=0){
                cdr->update_failed_resource(*ri);
                throw AmSession::Exception(refuse_code,refuse_reason);
            }

            ParamReplacerCtx rctx(profile);
            if(router.check_and_refuse(profile,cdr,aleg_modified_req,rctx)){
                throw AmSession::Exception(cdr->disconnect_rewrited_code,
                                           cdr->disconnect_rewrited_reason);
            }
        }
        attempt++;
    } while(rctl_ret != RES_CTL_OK);

    if(rctl_ret != RES_CTL_OK){
        throw AmSession::Exception(refuse_code,refuse_reason);
    }

    PROF_END(rchk);
    PROF_PRINT("check and grab resources",rchk);

    profile = call_ctx->getCurrentProfile();
    cdr->update(profile->rl);
    updateCallProfile(*profile);

    PROF_START(sdp_processing);

    //filterSDP
    int res = processSdpOffer(call_profile,
                              aleg_modified_req.body, aleg_modified_req.method,
                              call_ctx->aleg_negotiated_media,
                              call_profile.static_codecs_aleg_id);
    if(res < 0){
        INFO("%s() Not acceptable codecs",FUNC_NAME);
        throw InternalException(FC_CODECS_NOT_MATCHED);
    }

    //next we should filter request for legB
    res = filterSdpOffer(this,
                         call_profile,
                         modified_req.body,modified_req.method,
                         call_profile.static_codecs_bleg_id,
                         &call_ctx->bleg_initial_offer);
    if(res < 0){
        INFO("%s() Not acceptable codecs for legB",FUNC_NAME);
        throw AmSession::Exception(488, SIP_REPLY_NOT_ACCEPTABLE_HERE);
    }
    PROF_END(sdp_processing);
    PROF_PRINT("initial sdp processing",sdp_processing);

    if(cdr->time_limit){
        DBG("%s() save timer %d with timeout %d",FUNC_NAME,
            YETI_CALL_DURATION_TIMER,
            cdr->time_limit);
        saveCallTimer(YETI_CALL_DURATION_TIMER,cdr->time_limit);
    }

    if(0!=cdr_list.insert(cdr)){
        ERROR("onInitialInvite(): double insert into active calls list. integrity threat");
        ERROR("ctx: attempt = %d, cdr.logger_path = %s",
            call_ctx->attempt_num,cdr->msg_logger_path.c_str());
        log_stacktrace(L_ERR);
        throw AmSession::Exception(500,SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    if(!call_profile.append_headers.empty()){
        replace(call_profile.append_headers,"%global_tag",getGlobalTag());
    }

    onRoutingReady();

    } catch(InternalException &e) {
        DBG("%s() catched InternalException(%d)",FUNC_NAME,
            e.icode);
        rctl.put(call_profile.resource_handler);
        cdr->update_internal_reason(DisconnectByTS,e.internal_reason,e.internal_code);
        throw AmSession::Exception(e.response_code,e.response_reason);
    } catch(AmSession::Exception &e) {
        DBG("%s() catched AmSession::Exception(%d,%s)",FUNC_NAME,
            e.code,e.reason.c_str());
        rctl.put(call_profile.resource_handler);
        cdr->update_internal_reason(DisconnectByTS,e.reason,e.code);
        throw e;
    }

    PROF_END(func);
    PROF_PRINT("yeti onRoutingReady()",func);
    return;
}

bool SBCCallLeg::chooseNextProfile(){
    DBG("%s()",FUNC_NAME);

    string refuse_reason;
    int refuse_code;
    CallCtx *ctx;
    Cdr *cdr;
    SqlCallProfile *profile = NULL;
    ResourceCtlResponse rctl_ret;
    ResourceList::iterator ri;
    bool has_profile = false;

    cdr = call_ctx->cdr;
    profile = call_ctx->getNextProfile(false);

    if(NULL==profile){
        //pretend that nothing happen. we were never called
        DBG("%s() no more profiles or refuse profile on serial fork. ignore it",FUNC_NAME);
        return false;
    }

    //write cdr and replace ctx pointer with new
    cdr_list.erase(cdr);
    router.write_cdr(cdr,false);
    cdr = call_ctx->cdr;

    do {
        DBG("%s() choosed next profile. check it for refuse",FUNC_NAME);

        ParamReplacerCtx rctx(profile);
        if(router.check_and_refuse(profile,cdr,*call_ctx->initial_invite,rctx)){
            DBG("%s() profile contains refuse code",FUNC_NAME);
            break;
        }

        DBG("%s() no refuse field. check it for resources",FUNC_NAME);
        ResourceList &rl = profile->rl;
        if(rl.empty()){
            rctl_ret = RES_CTL_OK;
        } else {
            rctl_ret = rctl.get(rl,
                                profile->resource_handler,
                                getLocalTag(),
                                refuse_code,refuse_reason,ri);
        }

        if(rctl_ret == RES_CTL_OK){
            DBG("%s() check resources  successed",FUNC_NAME);
            has_profile = true;
            break;
        } else {
            DBG("%s() check resources failed with code: %d, reply: <%d '%s'>",FUNC_NAME,
                rctl_ret,refuse_code,refuse_reason.c_str());
            if(rctl_ret ==  RES_CTL_ERROR) {
                break;
            } else if(rctl_ret ==  RES_CTL_REJECT) {
                cdr->update_failed_resource(*ri);
                break;
            } else if(	rctl_ret == RES_CTL_NEXT){
                profile = ctx->getNextProfile(false,true);
                if(NULL==profile){
                    cdr->update_failed_resource(*ri);
                    DBG("%s() there are no profiles more",FUNC_NAME);
                    break;
                }
                if(profile->disconnect_code_id!=0){
                    cdr->update_failed_resource(*ri);
                    DBG("%s() failovered to refusing profile %d",FUNC_NAME,
                        profile->disconnect_code_id);
                    break;
                }
            }
        }
    } while(rctl_ret != RES_CTL_OK);

    if(!has_profile){
        cdr->update_internal_reason(DisconnectByTS,refuse_reason,refuse_code);
        return false;
    } else {
        DBG("%s() update call profile for legA",FUNC_NAME);
        cdr->update(profile->rl);
        updateCallProfile(*profile);
        return true;
    }
}

bool SBCCallLeg::connectCallee(const AmSipRequest &orig_req)
{
    ParamReplacerCtx ctx(&call_profile);
    ctx.app_param = getHeader(orig_req.hdrs, PARAM_HDR, true);

    AmSipRequest uac_req(orig_req);
    AmUriParser uac_ruri;

    uac_ruri.uri = uac_req.r_uri;
    if(!uac_ruri.parse_uri()) {
        DBG("Error parsing R-URI '%s'\n",uac_ruri.uri.c_str());
        throw AmSession::Exception(400,"Failed to parse R-URI");
    }

    call_profile.sst_aleg_enabled = ctx.replaceParameters(
        call_profile.sst_aleg_enabled,
        "enable_aleg_session_timer",
        orig_req
    );

    call_profile.sst_enabled = ctx.replaceParameters(
        call_profile.sst_enabled,
        "enable_session_timer", orig_req
    );

    if ((call_profile.sst_aleg_enabled == "yes") ||
        (call_profile.sst_enabled == "yes"))
    {
        call_profile.eval_sst_config(ctx,orig_req,call_profile.sst_a_cfg);
        if(applySSTCfg(call_profile.sst_a_cfg,&orig_req) < 0) {
            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
    }


    if (!call_profile.evaluate(ctx, orig_req)) {
        ERROR("call profile evaluation failed\n");
        throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }
    if(!call_profile.append_headers.empty()){
        replace(call_profile.append_headers,"%global_tag",getGlobalTag());
    }

    if(call_profile.contact_hiding) {
        if(RegisterDialog::decodeUsername(orig_req.user,uac_ruri)) {
            uac_req.r_uri = uac_ruri.uri_str();
        }
    } else if(call_profile.reg_caching) {
        // REG-Cache lookup
        uac_req.r_uri = call_profile.retarget(orig_req.user,*dlg);
    }

    string ruri, to, from;

    ruri = call_profile.ruri.empty() ? uac_req.r_uri : call_profile.ruri;
    if(!call_profile.ruri_host.empty()){
        ctx.ruri_parser.uri = ruri;
        if(!ctx.ruri_parser.parse_uri()) {
            WARN("Error parsing R-URI '%s'\n", ruri.c_str());
        } else {
            ctx.ruri_parser.uri_port.clear();
            ctx.ruri_parser.uri_host = call_profile.ruri_host;
            ruri = ctx.ruri_parser.uri_str();
        }
    }
    from = call_profile.from.empty() ? orig_req.from : call_profile.from;
    to = call_profile.to.empty() ? orig_req.to : call_profile.to;

    applyAProfile();
    call_profile.apply_a_routing(ctx,orig_req,*dlg);

    AmSipRequest invite_req(orig_req);

    removeHeader(invite_req.hdrs,PARAM_HDR);
    removeHeader(invite_req.hdrs,"P-App-Name");

    if (call_profile.sst_enabled_value) {
        removeHeader(invite_req.hdrs,SIP_HDR_SESSION_EXPIRES);
        removeHeader(invite_req.hdrs,SIP_HDR_MIN_SE);
    }

    size_t start_pos = 0;
    while (start_pos<call_profile.append_headers.length()) {
        int res;
        size_t name_end, val_begin, val_end, hdr_end;
        if ((res = skip_header(call_profile.append_headers, start_pos, name_end, val_begin,
                val_end, hdr_end)) != 0) {
            ERROR("skip_header for '%s' pos: %ld, return %d",
                    call_profile.append_headers.c_str(),start_pos,res);
            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
        string hdr_name = call_profile.append_headers.substr(start_pos, name_end-start_pos);
        while(!getHeader(invite_req.hdrs,hdr_name).empty()){
            removeHeader(invite_req.hdrs,hdr_name);
        }
        start_pos = hdr_end;
    }

    inplaceHeaderPatternFilter(invite_req.hdrs, call_profile.headerfilter_a2b);

    if (call_profile.append_headers.size() > 2) {
        string append_headers = call_profile.append_headers;
        assertEndCRLF(append_headers);
        invite_req.hdrs+=append_headers;
    }

    int res = filterSdpOffer(this,
                             call_profile,
                             invite_req.body,invite_req.method,
                             call_profile.static_codecs_bleg_id,
                             &call_ctx->bleg_initial_offer);
    if(res < 0){
        INFO("onInitialInvite() Not acceptable codecs for legB");
        throw AmSession::Exception(488, SIP_REPLY_NOT_ACCEPTABLE_HERE);
    }

    connectCallee(to, ruri, from, orig_req, invite_req);

    return false;
}

void SBCCallLeg::onRadiusReply(const RadiusReplyEvent &ev)
{
    DBG("got radius reply for %s",getLocalTag().c_str());

    if(AmBasicSipDialog::Cancelling==dlg->getStatus()) {
        DBG("[%s] ignore radius reply in Cancelling state",getLocalTag().c_str());
        return;
    }
    getCtx_void
    try {
        switch(ev.result){
        case RadiusReplyEvent::Accepted:
            processRouting();
            break;
        case RadiusReplyEvent::Rejected:
            throw InternalException(RADIUS_RESPONSE_REJECT);
            break;
        case RadiusReplyEvent::Error:
            if(ev.reject_on_error){
                ERROR("[%s] radius error %d. reject",
                    getLocalTag().c_str(),ev.error_code);
                throw InternalException(ev.error_code);
            } else {
                ERROR("[%s] radius error %d, but radius profile configured to ignore errors.",
                    getLocalTag().c_str(),ev.error_code);
                processRouting();
            }
            break;
        }
    } catch(AmSession::Exception &e) {
        onEarlyEventException(e.code,e.reason);
    } catch(InternalException &e){
        onEarlyEventException(e.response_code,e.response_reason);
    }
}

void SBCCallLeg::onRtpTimeoutOverride(const AmRtpTimeoutEvent &rtp_event)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    unsigned int internal_code,response_code;
    string internal_reason,response_reason;

    getCtx_void

    if(getCallStatus()!=CallLeg::Connected){
        WARN("%s: module catched RtpTimeout in no Connected state. ignore it",
             getLocalTag().c_str());
        return;
    }

    CodesTranslator::instance()->translate_db_code(
        DC_RTP_TIMEOUT,
        internal_code,internal_reason,
        response_code,response_reason,
        call_ctx->getOverrideId());
    with_cdr_for_read {
        cdr->update_internal_reason(DisconnectByTS,internal_reason,internal_code);
        cdr->update_aleg_reason("Bye",200);
        cdr->update_bleg_reason("Bye",200);
    }
}

void SBCCallLeg::onTimerEvent(int timer_id)
{
    DBG("%s(%p,%d,leg%s)",FUNC_NAME,this,timer_id,a_leg?"A":"B");
    getCtx_void
    with_cdr_for_read {
        switch(timer_id){
        case YETI_CALL_DURATION_TIMER:
            cdr->update_internal_reason(DisconnectByTS,"Call duration limit reached",200);
            cdr->update_aleg_reason("Bye",200);
            cdr->update_bleg_reason("Bye",200);
            break;
        case YETI_RINGING_TIMEOUT_TIMER:
            call_ctx->setRingingTimeout();
            dlg->cancel();
            break;
        case YETI_RADIUS_INTERIM_TIMER:
            onInterimRadiusTimer();
            return;
        case YETI_FAKE_RINGING_TIMER:
            onFakeRingingTimer();
            return;
        default:
            cdr->update_internal_reason(DisconnectByTS,"Timer "+int2str(timer_id)+" fired",200);
            break;
        }
    }
}

void SBCCallLeg::onInterimRadiusTimer()
{
    DBG("interim accounting timer fired for %s",getLocalTag().c_str());
    getCtx_void
    with_cdr_for_read {
        radius_accounting_interim(this,*cdr);
    }
}

void SBCCallLeg::onFakeRingingTimer()
{
    DBG("fake ringing timer fired for %s",getLocalTag().c_str());
    getCtx_void
    if(!call_ctx->ringing_sent) {
        dlg->reply(*call_ctx->initial_invite,180,SIP_REPLY_RINGING);
        call_ctx->ringing_sent = true;
    }
}

void SBCCallLeg::onControlEvent(SBCControlEvent *event)
{
    DBG("%s(%p,leg%s) cmd = %s, event_id = %d",FUNC_NAME,this,a_leg?"A":"B",
        event->cmd.c_str(),event->event_id);
    if(event->cmd=="teardown"){
        onTearDown();
    }
}

void SBCCallLeg::onTearDown()
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    getCtx_void
    with_cdr_for_read {
        cdr->update_internal_reason(DisconnectByTS,"Teardown",200);
        cdr->update_aleg_reason("Bye",200);
        cdr->update_bleg_reason("Bye",200);
    }
}

void SBCCallLeg::onSystemEventOverride(AmSystemEvent* event)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    if (event->sys_event == AmSystemEvent::ServerShutdown) {
        onServerShutdown();
    }
}

void SBCCallLeg::onServerShutdown()
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    getCtx_void
    with_cdr_for_read {
        cdr->update_internal_reason(DisconnectByTS,"ServerShutdown",200);
    }
    //may never reach onDestroy callback so free resources here
    rctl.put(call_profile.resource_handler);
}

void SBCCallLeg::onStart()
{
    // this should be the first thing called in session's thread
    CallLeg::onStart();
    if (!a_leg) applyBProfile(); // A leg needs to evaluate profile first
    else if (!getOtherId().empty()) {
        // A leg but we already have a peer, what means that this call leg was
        // created as an A leg for already existing B leg (for example call
        // transfer)
        // we need to apply a profile, we use B profile and understand it as an
        // "outbound" profile though we are in A leg
        applyBProfile();
    }
}

void SBCCallLeg::updateCallProfile(const SBCCallProfile &new_profile)
{
    call_profile = new_profile;
    placeholders_hash.update(call_profile.placeholders_hash);
}

void SBCCallLeg::applyAProfile()
{
    // apply A leg configuration (but most of the configuration is applied in
    // SBCFactory::onInvite)

    setAllow1xxWithoutToTag(call_profile.allow_1xx_without_to_tag);

    if (call_profile.rtprelay_enabled || call_profile.transcoder.isActive()) {
        DBG("Enabling RTP relay mode for SBC call\n");

        setRtpRelayForceSymmetricRtp(call_profile.aleg_force_symmetric_rtp_value);
        DBG("%s\n",getRtpRelayForceSymmetricRtp() ?
            "forcing symmetric RTP (passive mode)":
            "disabled symmetric RTP (normal mode)");
        setRtpEndlessSymmetricRtp(call_profile.bleg_symmetric_rtp_nonstop);
        setRtpSymmetricRtpIgnoreRTCP(call_profile.bleg_symmetric_rtp_ignore_rtcp);

        if (call_profile.aleg_rtprelay_interface_value >= 0) {
            setRtpInterface(call_profile.aleg_rtprelay_interface_value);
            DBG("using RTP interface %i for A leg\n", rtp_interface);
        }

        setRtpRelayTransparentSeqno(call_profile.rtprelay_transparent_seqno);
        setRtpRelayTransparentSSRC(call_profile.rtprelay_transparent_ssrc);
        setRtpRelayTimestampAligning(call_profile.relay_timestamp_aligning);
        setEnableDtmfRtpFiltering(call_profile.rtprelay_dtmf_filtering);
        setEnableDtmfRtpDetection(call_profile.rtprelay_dtmf_detection);
        setEnableDtmfForceRelay(call_profile.rtprelay_force_dtmf_relay);
        setEnableCNForceRelay(call_profile.force_relay_CN);
        setEnableRtpPing(call_profile.aleg_rtp_ping);
        setRtpTimeout(call_profile.dead_rtp_time);
        setIgnoreRelayStreams(call_profile.filter_noaudio_streams);

        if(call_profile.transcoder.isActive()) {
            setRtpRelayMode(RTP_Transcoding);
            switch(call_profile.transcoder.dtmf_mode) {
            case SBCCallProfile::TranscoderSettings::DTMFAlways:
                enable_dtmf_transcoding = true; break;
            case SBCCallProfile::TranscoderSettings::DTMFNever:
                enable_dtmf_transcoding = false; break;
            case SBCCallProfile::TranscoderSettings::DTMFLowFiCodecs:
                enable_dtmf_transcoding = false;
                lowfi_payloads = call_profile.transcoder.lowfi_codecs;
                break;
            };
        } else {
            setRtpRelayMode(RTP_Relay);
        }
        // copy stats counters
        rtp_pegs = call_profile.aleg_rtp_counters;
    }

    if(!call_profile.dlg_contact_params.empty())
        dlg->setContactParams(call_profile.dlg_contact_params);
}

int SBCCallLeg::applySSTCfg(AmConfigReader& sst_cfg, const AmSipRequest *p_req)
{
    DBG("Enabling SIP Session Timers\n");
    if (NULL == SBCFactory::instance()->session_timer_fact) {
        ERROR("session_timer module not loaded - "
              "unable to create call with SST\n");
        return -1;
    }

    if (p_req && !SBCFactory::instance()->session_timer_fact->
        onInvite(*p_req, sst_cfg)) {
        return -1;
    }

    AmSessionEventHandler* h = SBCFactory::instance()->session_timer_fact->getHandler(this);
    if (!h) {
        ERROR("could not get a session timer event handler\n");
        return -1;
    }

    if (h->configure(sst_cfg)) {
        ERROR("Could not configure the session timer: "
              "disabling session timers.\n");
        delete h;
    } else {
        addHandler(h);
        // hack: repeat calling the handler again to start timers because
        // it was called before SST was applied
        if(p_req) h->onSipRequest(*p_req);
    }

    return 0;
}

void SBCCallLeg::applyBProfile()
{
    // TODO: fix this!!! (see d85ed5c7e6b8d4c24e7e5b61c732c2e1ddd31784)
    // if (!call_profile.contact.empty()) {
    //   dlg->contact_uri = SIP_HDR_COLSP(SIP_HDR_CONTACT) + call_profile.contact + CRLF;
    // }

    setAllow1xxWithoutToTag(call_profile.allow_1xx_without_to_tag);

    if (call_profile.auth_enabled) {
        // adding auth handler
        AmSessionEventHandlerFactory* uac_auth_f =
            AmPlugIn::instance()->getFactory4Seh("uac_auth");
        if (NULL == uac_auth_f)  {
            INFO("uac_auth module not loaded. uac auth NOT enabled.\n");
        } else {
            AmSessionEventHandler* h = uac_auth_f->getHandler(this);

            // we cannot use the generic AmSessi(onEvent)Handler hooks,
            // because the hooks don't work in AmB2BSession
            setAuthHandler(h);
            DBG("uac auth enabled for callee session.\n");
        }
    }

    if (call_profile.sst_enabled_value) {
        if(applySSTCfg(call_profile.sst_b_cfg,NULL) < 0) {
            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
    }

    if (!call_profile.outbound_proxy.empty()) {
        dlg->outbound_proxy = call_profile.outbound_proxy;
        dlg->force_outbound_proxy = call_profile.force_outbound_proxy;
    }

    if (!call_profile.next_hop.empty()) {
        DBG("set next hop to '%s' (1st_req=%s,fixed=%s)\n",
            call_profile.next_hop.c_str(), call_profile.next_hop_1st_req?"true":"false",
            call_profile.next_hop_fixed?"true":"false");
        dlg->setNextHop(call_profile.next_hop);
        dlg->setNextHop1stReq(call_profile.next_hop_1st_req);
        dlg->setNextHopFixed(call_profile.next_hop_fixed);
    }

    DBG("patch_ruri_next_hop = %i",call_profile.patch_ruri_next_hop);
    dlg->setPatchRURINextHop(call_profile.patch_ruri_next_hop);

    // was read from caller but reading directly from profile now
    if (call_profile.outbound_interface_value >= 0)
        dlg->setOutboundInterface(call_profile.outbound_interface_value);

    // was read from caller but reading directly from profile now
    if (call_profile.rtprelay_enabled || call_profile.transcoder.isActive()) {

        if (call_profile.rtprelay_interface_value >= 0)
            setRtpInterface(call_profile.rtprelay_interface_value);

        setRtpRelayForceSymmetricRtp(call_profile.force_symmetric_rtp_value);
        DBG("%s\n",getRtpRelayForceSymmetricRtp() ?
            "forcing symmetric RTP (passive mode)":
            "disabled symmetric RTP (normal mode)");
        setRtpEndlessSymmetricRtp(call_profile.bleg_symmetric_rtp_nonstop);
        setRtpSymmetricRtpIgnoreRTCP(call_profile.bleg_symmetric_rtp_ignore_rtcp);

        setRtpRelayTransparentSeqno(call_profile.rtprelay_transparent_seqno);
        setRtpRelayTransparentSSRC(call_profile.rtprelay_transparent_ssrc);
        setRtpRelayTimestampAligning(call_profile.relay_timestamp_aligning);
        setEnableDtmfRtpFiltering(call_profile.rtprelay_dtmf_filtering);
        setEnableDtmfRtpDetection(call_profile.rtprelay_dtmf_detection);
        setEnableDtmfForceRelay(call_profile.rtprelay_force_dtmf_relay);
        setEnableCNForceRelay(call_profile.force_relay_CN);
        setEnableRtpPing(call_profile.bleg_rtp_ping);
        setRtpTimeout(call_profile.dead_rtp_time);
        setIgnoreRelayStreams(call_profile.filter_noaudio_streams);

        // copy stats counters
        rtp_pegs = call_profile.bleg_rtp_counters;
    }

    // was read from caller but reading directly from profile now
    if (!call_profile.callid.empty())
        dlg->setCallid(call_profile.callid);

    if(!call_profile.bleg_dlg_contact_params.empty())
        dlg->setContactParams(call_profile.bleg_dlg_contact_params);

    setInviteTransactionTimeout(call_profile.inv_transaction_timeout);
    setInviteRetransmitTimeout(call_profile.inv_srv_failover_timeout);
}

int SBCCallLeg::relayEvent(AmEvent* ev)
{
    if(NULL==call_ctx) {
        ERROR("Yeti::relayEvent(%p) zero ctx. ignore event",this);
        return -1;
    }

    AmOfferAnswer::OAState dlg_oa_state = dlg->getOAState();

    switch (ev->event_id) {
    case B2BSipRequest: {
        B2BSipRequestEvent* req_ev = dynamic_cast<B2BSipRequestEvent*>(ev);
        assert(req_ev);

        AmSipRequest &req = req_ev->req;

        DBG("Yeti::relayEvent(%p) filtering request '%s' (c/t '%s') oa_state = %d\n",
            this,req.method.c_str(), req.body.getCTStr().c_str(),
            dlg_oa_state);

        try {
            int res;
            if(req.method==SIP_METH_ACK){
                //ACK can contain only answer
                dump_SdpMedia(call_ctx->bleg_negotiated_media,"bleg_negotiated media_pre");
                dump_SdpMedia(call_ctx->aleg_negotiated_media,"aleg_negotiated media_pre");

                res = processSdpAnswer(
                    this,
                    req.body, req.method,
                    call_ctx->get_other_negotiated_media(a_leg),
                    a_leg ? call_profile.bleg_single_codec : call_profile.aleg_single_codec,
                    call_profile.filter_noaudio_streams,
                    //ACK request MUST contain SDP answer if we sent offer in reply
                    dlg_oa_state==AmOfferAnswer::OA_OfferSent
                );

                dump_SdpMedia(call_ctx->bleg_negotiated_media,"bleg_negotiated media_post");
                dump_SdpMedia(call_ctx->aleg_negotiated_media,"aleg_negotiated media_post");

            } else {
                res = processSdpOffer(
                    call_profile,
                    req.body, req.method,
                    call_ctx->get_self_negotiated_media(a_leg),
                    a_leg ? call_profile.static_codecs_aleg_id : call_profile.static_codecs_bleg_id
                );
                if(res>=0){
                    res = filterSdpOffer(
                        this,
                        call_profile,
                        req.body, req.method,
                        a_leg ? call_profile.static_codecs_bleg_id : call_profile.static_codecs_aleg_id
                    );
                }
            }
            if (res < 0) {
                delete ev;
                return res;
            }
        } catch(InternalException &exception){
            DBG("got internal exception %d on request processing",exception.icode);
            delete ev;
            return -448;
        }

        inplaceHeaderPatternFilter(
            req.hdrs,
            a_leg ? call_profile.headerfilter_a2b : call_profile.headerfilter_b2a);

        if((a_leg && call_profile.keep_vias)
            || (!a_leg && call_profile.bleg_keep_vias))
        {
            req.hdrs = req.vias +req.hdrs;
        }
    } break;
    case B2BSipReply: {
        B2BSipReplyEvent* reply_ev = dynamic_cast<B2BSipReplyEvent*>(ev);
        assert(reply_ev);

        AmSipReply &reply = reply_ev->reply;

        DBG("Yeti::relayEvent(%p) filtering body for reply %d cseq.method '%s' (c/t '%s') oa_state = %d\n",
            this,reply.code,reply_ev->trans_method.c_str(), reply.body.getCTStr().c_str(),
            dlg_oa_state);

        //append headers for 200 OK reply in direction B -> A
        inplaceHeaderPatternFilter(
            reply.hdrs,
            a_leg ? call_profile.headerfilter_a2b : call_profile.headerfilter_b2a
        );

        do {
            if(!a_leg){
                if(reply.code==200
                   && !call_profile.aleg_append_headers_reply.empty())
                {
                    size_t start_pos = 0;
                    while (start_pos<call_profile.aleg_append_headers_reply.length()) {
                        int res;
                        size_t name_end, val_begin, val_end, hdr_end;
                        if ((res = skip_header(call_profile.aleg_append_headers_reply, start_pos, name_end, val_begin,
                                val_end, hdr_end)) != 0) {
                            ERROR("skip_header for '%s' pos: %ld, return %d",
                                    call_profile.aleg_append_headers_reply.c_str(),start_pos,res);
                            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
                        }
                        string hdr_name = call_profile.aleg_append_headers_reply.substr(start_pos, name_end-start_pos);
                        start_pos = hdr_end;
                        while(!getHeader(reply.hdrs,hdr_name).empty()){
                            removeHeader(reply.hdrs,hdr_name);
                        }
                    }
                    assertEndCRLF(call_profile.aleg_append_headers_reply);
                    reply.hdrs+=call_profile.aleg_append_headers_reply;
                }

                if(call_profile.suppress_early_media
                    && reply.code>=180
                    && reply.code < 190)
                {
                    DBG("convert B->A reply %d %s to %d %s and clear body",
                        reply.code,reply.reason.c_str(),
                        180,SIP_REPLY_RINGING);

                    //patch code and reason
                    reply.code = 180;
                    reply.reason = SIP_REPLY_RINGING;
                    //сlear body
                    reply.body.clear();
                    break;
                }
            }

            try {
                int res;
                if(dlg_oa_state==AmOfferAnswer::OA_OfferRecved){
                    DBG("relayEvent(): process offer in reply");
                    res = processSdpOffer(
                        call_profile,
                        reply.body, reply.cseq_method,
                        call_ctx->get_self_negotiated_media(a_leg),
                        a_leg ? call_profile.static_codecs_aleg_id : call_profile.static_codecs_bleg_id,
                        false,
                        a_leg ? call_profile.aleg_single_codec : call_profile.bleg_single_codec
                    );
                    if(res>=0){
                        res = filterSdpOffer(
                            this,
                            call_profile,
                            reply.body, reply.cseq_method,
                            a_leg ? call_profile.static_codecs_bleg_id : call_profile.static_codecs_aleg_id
                        );
                    }
                } else {
                    DBG("relayEvent(): process asnwer in reply");
                    res = processSdpAnswer(
                        this,
                        reply.body, reply.cseq_method,
                        call_ctx->get_other_negotiated_media(a_leg),
                        a_leg ? call_profile.bleg_single_codec : call_profile.aleg_single_codec,
                        call_profile.filter_noaudio_streams,
                        //final positive reply MUST contain SDP answer if we sent offer
                        (dlg_oa_state==AmOfferAnswer::OA_OfferSent
                            && reply.code >= 200 && reply.code < 300)
                    );
                }

                if(res<0){
                    terminateLegOnReplyException(reply,InternalException(DC_REPLY_SDP_GENERIC_EXCEPTION));
                    delete ev;
                    return -488;
                }
            } catch(InternalException &exception){
                DBG("got internal exception %d on reply processing",exception.icode);
                terminateLegOnReplyException(reply,exception);
                delete ev;
                return -488;
            }
        } while(0);

        //yeti_part
        if(call_profile.transparent_dlg_id &&
           (reply_ev->reply.from_tag == dlg->getExtLocalTag()))
           reply_ev->reply.from_tag = dlg->getLocalTag();

    } break;
    } //switch (ev->event_id)
    return CallLeg::relayEvent(ev);
}

SBCCallLeg::~SBCCallLeg()
{
    if (auth)
        delete auth;
    if (logger) dec_ref(logger);
    if(sensor) dec_ref(sensor);
}

void SBCCallLeg::onBeforeDestroy()
{
    DBG("%s(%p|%s,leg%s)",FUNC_NAME,
        this,getLocalTag().c_str(),a_leg?"A":"B");

    CallCtx *ctx = call_ctx;
    if(!ctx) return;

    call_ctx->lock();
    call_ctx = NULL;

    if(call_profile.record_audio) {
        AmAudioFileRecorderProcessor::instance()->removeRecorder(getLocalTag());
    }

    if(ctx->dec_and_test()) {
        DBG("last leg destroy");
        SqlCallProfile *p = ctx->getCurrentProfile();
        if(NULL!=p) rctl.put(p->resource_handler);
        Cdr *cdr = ctx->cdr;
        if(cdr) {
            cdr_list.erase(cdr);
            router.write_cdr(cdr,true);
        }
        ctx->unlock();
        delete ctx;
    } else {
        ctx->unlock();
    }
}

UACAuthCred* SBCCallLeg::getCredentials()
{
    if (a_leg) return &call_profile.auth_aleg_credentials;
    else return &call_profile.auth_credentials;
}

void SBCCallLeg::onSipRequest(const AmSipRequest& req)
{
    // AmB2BSession does not call AmSession::onSipRequest for
    // forwarded requests - so lets call event handlers here
    // todo: this is a hack, replace this by calling proper session
    // event handler in AmB2BSession
    bool fwd = sip_relay_only && (req.method != SIP_METH_CANCEL);
    if (fwd) {
        CALL_EVENT_H(onSipRequest,req);
    }

    if (fwd && call_profile.messagefilter.size()) {
        for (vector<FilterEntry>::iterator it=call_profile.messagefilter.begin();
             it != call_profile.messagefilter.end(); it++)
        {
            if (isActiveFilter(it->filter_type)) {
                bool is_filtered = (it->filter_type == Whitelist) ^
                     (it->filter_list.find(req.method) != it->filter_list.end());
                if (is_filtered) {
                    DBG("replying 405 to filtered message '%s'\n", req.method.c_str());
                    dlg->reply(req, 405, "Method Not Allowed", NULL, "", SIP_FLAGS_VERBATIM);
                    return;
                }
            }
        }
    }

    do {
        if(!call_ctx->initial_invite)
            break;

        DBG("onInDialogRequest(%p|%s,leg%s) '%s'",this,getLocalTag().c_str(),a_leg?"A":"B",req.method.c_str());

        if(req.method == SIP_METH_OPTIONS
            && ((a_leg && !call_profile.aleg_relay_options)
                || (!a_leg && !call_profile.bleg_relay_options)))
        {
            dlg->reply(req, 200, "OK", NULL, "", SIP_FLAGS_VERBATIM);
            return;
        } else if(req.method == SIP_METH_UPDATE
                  && ((a_leg && !call_profile.aleg_relay_update)
                      || (!a_leg && !call_profile.bleg_relay_update)))
        {
            getCtx_chained;

            const AmMimeBody* sdp_body = req.body.hasContentType(SIP_APPLICATION_SDP);
            if(!sdp_body){
                DBG("got UPDATE without body. local processing enabled. generate 200OK without SDP");
                AmSipRequest upd_req(req);
                processLocalRequest(upd_req);
                return;
            }

            AmSdp sdp;
            int res = sdp.parse((const char *)sdp_body->getPayload());
            if(0 != res) {
                DBG("SDP parsing failed: %d. respond with 488\n",res);
                dlg->reply(req,488,"Not Acceptable Here");
                return;
            }

            AmSipRequest upd_req(req);
            try {
                int res = processSdpOffer(
                    call_profile,
                    upd_req.body, upd_req.method,
                    call_ctx->get_self_negotiated_media(a_leg),
                    a_leg ? call_profile.static_codecs_aleg_id : call_profile.static_codecs_bleg_id,
                    true,
                    a_leg ? call_profile.aleg_single_codec : call_profile.bleg_single_codec
                );
                if (res < 0) {
                    dlg->reply(req,488,"Not Acceptable Here");
                    return;
                }
            } catch(InternalException &e){
                dlg->reply(req,e.response_code,e.response_reason);
                return;
            }

            processLocalRequest(upd_req);
            return;
        } else if(req.method == SIP_METH_PRACK
                  && ((a_leg && !call_profile.aleg_relay_prack)
                      || (!a_leg && !call_profile.bleg_relay_prack)))
        {
            dlg->reply(req,200, "OK", NULL, "", SIP_FLAGS_VERBATIM);
            return;
        } else if(req.method == SIP_METH_INVITE)
        {
            getCtx_chained;

            if((a_leg && call_profile.aleg_relay_reinvite)
                || (!a_leg && call_profile.bleg_relay_reinvite))
            {
                DBG("skip local processing. relay");
                break;
            }

            const AmMimeBody* sdp_body = req.body.hasContentType(SIP_APPLICATION_SDP);
            if(!sdp_body){
                DBG("got reINVITE without body. local processing enabled. generate 200OK with SDP offer");
                DBG("replying 100 Trying to INVITE to be processed locally");
                dlg->reply(req, 100, SIP_REPLY_TRYING);
                AmSipRequest inv_req(req);
                processLocalRequest(inv_req);
                return;
            }

            AmSdp sdp;
            int res = sdp.parse((const char *)sdp_body->getPayload());
            if(0 != res) {
                DBG("replying 100 Trying to INVITE to be processed locally");
                dlg->reply(req, 100, SIP_REPLY_TRYING);
                DBG("SDP parsing failed: %d. respond with 488\n",res);
                dlg->reply(req,488,"Not Acceptable Here");
                return;
            }

            //check for hold/unhold request to pass them transparently
            HoldMethod method;
            if(isHoldRequest(sdp,method)){
                DBG("hold request matched. relay_hold = %d",
                    a_leg?call_profile.aleg_relay_hold:call_profile.bleg_relay_hold);

                if((a_leg && call_profile.aleg_relay_hold)
                    || (!a_leg && call_profile.bleg_relay_hold))
                {
                    DBG("skip local processing for hold request");
                    call_ctx->on_hold = true;
                    break;
                }
            } else if(call_ctx->on_hold){
                DBG("we in hold state. skip local processing for unhold request");
                call_ctx->on_hold = false;
                break;
            }

            DBG("replying 100 Trying to INVITE to be processed locally");
            dlg->reply(req, 100, SIP_REPLY_TRYING);

            AmSipRequest inv_req(req);
            try {
                int res = processSdpOffer(
                    call_profile,
                    inv_req.body, inv_req.method,
                    call_ctx->get_self_negotiated_media(a_leg),
                    a_leg ? call_profile.static_codecs_aleg_id : call_profile.static_codecs_bleg_id,
                    true,
                    a_leg ? call_profile.aleg_single_codec : call_profile.bleg_single_codec
                );
                if (res < 0) {
                    dlg->reply(req,488,"Not Acceptable Here");
                    return;
                }
            } catch(InternalException &e){
                dlg->reply(req,e.response_code,e.response_reason);
                return;
            }

            processLocalRequest(inv_req);
            return;
        }

        if(a_leg){
            if(req.method==SIP_METH_CANCEL){
                getCtx_chained;
                with_cdr_for_read {
                    cdr->update_internal_reason(DisconnectByORG,"Request terminated (Cancel)",487);
                }
            }
        }
    } while(0);

    if (fwd && req.method == SIP_METH_INVITE) {
        DBG("replying 100 Trying to INVITE to be fwd'ed\n");
        dlg->reply(req, 100, SIP_REPLY_TRYING);
    }

    CallLeg::onSipRequest(req);
}

void SBCCallLeg::setOtherId(const AmSipReply& reply)
{
    DBG("setting other_id to '%s'",reply.from_tag.c_str());
    setOtherId(reply.from_tag);
    if(call_profile.transparent_dlg_id && !reply.to_tag.empty()) {
        dlg->setExtLocalTag(reply.to_tag);
    }
}

void SBCCallLeg::onInitialReply(B2BSipReplyEvent *e)
{
    if (call_profile.transparent_dlg_id && !e->reply.to_tag.empty()
        && dlg->getStatus() != AmBasicSipDialog::Connected)
    {
        dlg->setExtLocalTag(e->reply.to_tag);
    }
    CallLeg::onInitialReply(e);
}

void SBCCallLeg::onSipReply(const AmSipRequest& req, const AmSipReply& reply,
			   AmBasicSipDialog::Status old_dlg_status)
{
    TransMap::iterator t = relayed_req.find(reply.cseq);
    bool fwd = t != relayed_req.end();

    DBG("onSipReply: %i %s (fwd=%i)\n",reply.code,reply.reason.c_str(),fwd);
    DBG("onSipReply: content-type = %s\n",reply.body.getCTStr().c_str());
    if (fwd) {
        CALL_EVENT_H(onSipReply, req, reply, old_dlg_status);
    }

    if (NULL != auth) {
        // only for SIP authenticated
        unsigned int cseq_before = dlg->cseq;
        if (auth->onSipReply(req, reply, old_dlg_status)) {
            if (cseq_before != dlg->cseq) {
                DBG("uac_auth consumed reply with cseq %d and resent with cseq %d; "
                    "updating relayed_req map\n", reply.cseq, cseq_before);
                updateUACTransCSeq(reply.cseq, cseq_before);
                // don't relay to other leg, process in AmSession
                AmSession::onSipReply(req, reply, old_dlg_status);
                // skip presenting reply to ext_cc modules, too
                return;
            }
        }
    }

    do {
        if(!a_leg) {
            getCtx_chained
            with_cdr_for_read {
                cdr->update(reply);
            }
        }
    } while(0);

    CallLeg::onSipReply(req, reply, old_dlg_status);
}

void SBCCallLeg::onSendRequest(AmSipRequest& req, int &flags)
{
    DBG("Yeti::onSendRequest(%p|%s) a_leg = %d",
        this,getLocalTag().c_str(),a_leg);

    if(call_ctx && !a_leg && req.method==SIP_METH_INVITE) {
        with_cdr_for_read cdr->update(BLegInvite);
    }

    if(a_leg) {
        if (!call_profile.aleg_append_headers_req.empty()) {
            size_t start_pos = 0;
            while (start_pos<call_profile.aleg_append_headers_req.length()) {
                int res;
                size_t name_end, val_begin, val_end, hdr_end;
                if ((res = skip_header(call_profile.aleg_append_headers_req, start_pos, name_end, val_begin,
                     val_end, hdr_end)) != 0)
                {
                    ERROR("skip_header for '%s' pos: %ld, return %d",
                        call_profile.aleg_append_headers_req.c_str(),start_pos,res);
                    throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
                }
                string hdr_name = call_profile.aleg_append_headers_req.substr(start_pos, name_end-start_pos);
                start_pos = hdr_end;
                while(!getHeader(req.hdrs,hdr_name).empty()){
                    removeHeader(req.hdrs,hdr_name);
                }
            }
            DBG("appending '%s' to outbound request (A leg)\n",
            call_profile.aleg_append_headers_req.c_str());
            req.hdrs+=call_profile.aleg_append_headers_req;
        }
    } else {
        size_t start_pos = 0;
        while (start_pos<call_profile.append_headers_req.length()) {
            int res;
            size_t name_end, val_begin, val_end, hdr_end;
            if ((res = skip_header(call_profile.append_headers_req, start_pos, name_end, val_begin,
                 val_end, hdr_end)) != 0)
            {
                ERROR("skip_header for '%s' pos: %ld, return %d",
                    call_profile.append_headers_req.c_str(),start_pos,res);
                throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
            }
            string hdr_name = call_profile.append_headers_req.substr(start_pos, name_end-start_pos);
            start_pos = hdr_end;
            while(!getHeader(req.hdrs,hdr_name).empty()){
                removeHeader(req.hdrs,hdr_name);
            }
        }
        if (!call_profile.append_headers_req.empty()) {
            DBG("appending '%s' to outbound request (B leg)\n",
                call_profile.append_headers_req.c_str());
            req.hdrs+=call_profile.append_headers_req;
        }
    }

    if (NULL != auth) {
        DBG("auth->onSendRequest cseq = %d\n", req.cseq);
        auth->onSendRequest(req, flags);
    }

    CallLeg::onSendRequest(req, flags);
}

void SBCCallLeg::onRemoteDisappeared(const AmSipReply& reply)
{
    const static string reinvite_failed("reINVITE failed");

    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    if(call_ctx) {
        if(a_leg){
            //trace available values
            if(call_ctx->initial_invite!=NULL) {
                AmSipRequest &req = *call_ctx->initial_invite;
                DBG("req.method = '%s'",req.method.c_str());
            } else {
                ERROR("intial_invite == NULL");
            }
            with_cdr_for_read {
                cdr->update_internal_reason(DisconnectByTS,reply.reason,reply.code);
            }
        }
        if(getCallStatus()==CallLeg::Connected) {
            with_cdr_for_read {
                cdr->update_internal_reason(
                    DisconnectByTS,
                    reinvite_failed, 200
                );
                cdr->update_aleg_reason("Bye",200);
                cdr->update_bleg_reason("Bye",200);
            }
        }
    }
    CallLeg::onRemoteDisappeared(reply);
}

void SBCCallLeg::onBye(const AmSipRequest& req)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    if(call_ctx) {
        with_cdr_for_read {
            if(a_leg){
                if(getCallStatus()!=CallLeg::Connected) {
                    ERROR("received Bye in not connected state");
                    cdr->update_internal_reason(DisconnectByORG,"EarlyBye",500);
                    cdr->update_aleg_reason("EarlyBye",200);
                    cdr->update_bleg_reason("Cancel",487);
                } else {
                    cdr->update_internal_reason(DisconnectByORG,"Bye",200);
                    cdr->update_bleg_reason("Bye",200);
                }
            } else {
                cdr->update_internal_reason(DisconnectByDST,"Bye",200);
                cdr->update_bleg_reason("Bye",200);
            }
        }
    }
    CallLeg::onBye(req);
}

void SBCCallLeg::onOtherBye(const AmSipRequest& req)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    if(call_ctx && a_leg) {
        if(getCallStatus()!=CallLeg::Connected) {
            //avoid considering of bye in not connected state as succ call
            ERROR("received OtherBye in not connected state");
            with_cdr_for_write {
                cdr->update_internal_reason(DisconnectByDST,"EarlyBye",500);
                cdr->update_aleg_reason("Request terminated",487);
                cdr_list.erase(cdr);
                router.write_cdr(cdr,true);
            }
        }
    }
    CallLeg::onOtherBye(req);
}

void SBCCallLeg::onDtmf(AmDtmfEvent* e)
{
    DBG("received DTMF on %c-leg (%i;%i)\n", a_leg ? 'A': 'B', e->event(), e->duration());

    AmSipDtmfEvent *sip_dtmf = NULL;
    int rx_proto = 0;
    bool allowed = false;
    struct timeval now;

    gettimeofday(&now, NULL);

    //filter incoming methods
    if((sip_dtmf = dynamic_cast<AmSipDtmfEvent *>(e))){
        DBG("received SIP DTMF event\n");
        allowed = a_leg ?
                    call_profile.aleg_dtmf_recv_modes&DTMF_RX_MODE_INFO :
                    call_profile.bleg_dtmf_recv_modes&DTMF_RX_MODE_INFO;
        rx_proto = DTMF_RX_MODE_INFO;
    /*} else if(dynamic_cast<AmRtpDtmfEvent *>(e)){
        DBG("RTP DTMF event\n");*/
    } else {
        DBG("received generic DTMF event\n");
        allowed = a_leg ?
                    call_profile.aleg_dtmf_recv_modes&DTMF_RX_MODE_RFC2833 :
                    call_profile.bleg_dtmf_recv_modes&DTMF_RX_MODE_RFC2833;
        rx_proto = DTMF_RX_MODE_RFC2833;
    }

    if(!allowed){
        DBG("DTMF event for leg %p rejected",this);
        e->processed = true;
        //write with zero tx_proto
        with_cdr_for_read cdr->add_dtmf_event(a_leg,e->event(),now,rx_proto,DTMF_TX_MODE_DISABLED);
        return;
    }

    //choose outgoing method
    int send_method = a_leg ?
                        call_profile.bleg_dtmf_send_mode_id :
                        call_profile.aleg_dtmf_send_mode_id;

    with_cdr_for_read cdr->add_dtmf_event(a_leg,e->event(),now,rx_proto,send_method);

    switch(send_method){
    case DTMF_TX_MODE_DISABLED:
        DBG("dtmf sending is disabled");
        break;
    case DTMF_TX_MODE_RFC2833: {
        DBG("send mode RFC2833 choosen for dtmf event for leg %p",this);
        AmB2BMedia *ms = getMediaSession();
        if(ms) {
            DBG("sending DTMF (%i;%i)\n", e->event(), e->duration());
            ms->sendDtmf(!a_leg,e->event(),e->duration());
        }
    } break;
    case DTMF_TX_MODE_INFO_DTMF_RELAY:
        DBG("send mode INFO/application/dtmf-relay choosen for dtmf event for leg %p",this);
        relayEvent(new yeti_dtmf::DtmfInfoSendEventDtmfRelay(e));
        break;
    case DTMF_TX_MODE_INFO_DTMF:
        DBG("send mode INFO/application/dtmf choosen for dtmf event for leg %p",this);
        relayEvent(new yeti_dtmf::DtmfInfoSendEventDtmf(e));
        break;
    default:
        ERROR("unknown dtmf send method %d. stop processing",send_method);
        break;
    }
}

void SBCCallLeg::updateLocalSdp(AmSdp &sdp)
{
    // anonymize SDP if configured to do so (we need to have our local media IP,
    // not the media IP of our peer leg there)
    if (call_profile.anonymize_sdp) normalizeSDP(sdp, call_profile.anonymize_sdp, advertisedIP());

    // remember transcodable payload IDs
    //if (call_profile.transcoder.isActive()) savePayloadIDs(sdp);
    CallLeg::updateLocalSdp(sdp);
}

void SBCCallLeg::onControlCmd(string& cmd, AmArg& params)
{
    if (cmd == "teardown") {
        if (a_leg) {
            // was for caller:
            DBG("teardown requested from control cmd\n");
            stopCall("ctrl-cmd");
            // FIXME: don't we want to relay the controll event as well?
        } else {
            // was for callee:
            DBG("relaying teardown control cmd to A leg\n");
            relayEvent(new SBCControlEvent(cmd, params));
            // FIXME: don't we want to stopCall as well?
        }
        return;
    }
    DBG("ignoring unknown control cmd : '%s'\n", cmd.c_str());
}


void SBCCallLeg::process(AmEvent* ev)
{
    DBG("%s(%p|%s,leg%s)",FUNC_NAME,this,
        getLocalTag().c_str(),a_leg?"A":"B");

    do {
        getCtx_chained
        RadiusReplyEvent *radius_event = dynamic_cast<RadiusReplyEvent*>(ev);
        if(radius_event){
            onRadiusReply(*radius_event);
            return;
        }

        AmRtpTimeoutEvent *rtp_event = dynamic_cast<AmRtpTimeoutEvent*>(ev);
        if(rtp_event){
            DBG("rtp event id: %d",rtp_event->event_id);
            onRtpTimeoutOverride(*rtp_event);
            return;
        }

        AmSipRequestEvent *request_event = dynamic_cast<AmSipRequestEvent*>(ev);
        if(request_event){
            AmSipRequest &req = request_event->req;
            DBG("request event method: %s",
                req.method.c_str());
        }

        AmSipReplyEvent *reply_event = dynamic_cast<AmSipReplyEvent*>(ev);
        if(reply_event){
            AmSipReply &reply = reply_event->reply;
            DBG("reply event  code: %d, reason:'%s'",
                reply.code,reply.reason.c_str());
            //!TODO: find appropiate way to avoid hangup in disconnected state
            if(reply.code==408 && getCallStatus()==CallLeg::Disconnected){
                DBG("received 408 in disconnected state. a_leg = %d, local_tag: %s",
                    a_leg, getLocalTag().c_str());
                throw AmSession::Exception(500,SIP_REPLY_SERVER_INTERNAL_ERROR);
            }
        }

        AmPluginEvent* plugin_event = dynamic_cast<AmPluginEvent*>(ev);
        if(plugin_event){
            DBG("%s plugin_event. name = %s, event_id = %d",FUNC_NAME,
                plugin_event->name.c_str(),
                plugin_event->event_id);
            if(plugin_event->name=="timer_timeout"){
                return onTimerEvent(plugin_event->data.get(0).asInt());
            }
        }

        SBCControlEvent* sbc_event = dynamic_cast<SBCControlEvent*>(ev);
        if(sbc_event){
            DBG("sbc event id: %d, cmd: %s",sbc_event->event_id,sbc_event->cmd.c_str());
            onControlEvent(sbc_event);
        }

        B2BEvent* b2b_e = dynamic_cast<B2BEvent*>(ev);
        if(b2b_e){
            if(b2b_e->event_id==B2BTerminateLeg){
                DBG("onEvent(%p|%s) terminate leg event",
                    this,getLocalTag().c_str());
            }
        }

        if (ev->event_id == E_SYSTEM) {
            AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
            if(sys_ev){
                DBG("sys event type: %d",sys_ev->sys_event);
                    onSystemEventOverride(sys_ev);
            }
        }

        yeti_dtmf::DtmfInfoSendEvent *dtmf = dynamic_cast<yeti_dtmf::DtmfInfoSendEvent*>(ev);
        if(dtmf) {
            DBG("onEvent dmtf(%d:%d)",dtmf->event(),dtmf->duration());
            dtmf->send(dlg);
            ev->processed = true;
            return;
        }
    } while(0);

    if (a_leg) {
        // was for caller (SBCDialog):
        AmPluginEvent* plugin_event = dynamic_cast<AmPluginEvent*>(ev);
        if(plugin_event && plugin_event->name == "timer_timeout") {
            int timer_id = plugin_event->data.get(0).asInt();
            if (timer_id >= SBC_TIMER_ID_CALL_TIMERS_START &&
                timer_id <= SBC_TIMER_ID_CALL_TIMERS_END)
            {
                DBG("timer %d timeout, stopping call\n", timer_id);
                stopCall("timer");
                ev->processed = true;
            }
        }

        SBCCallTimerEvent* ct_event;
        if (ev->event_id == SBCCallTimerEvent_ID &&
            (ct_event = dynamic_cast<SBCCallTimerEvent*>(ev)) != NULL)
        {
            switch (m_state) {
            case BB_Connected:
                switch (ct_event->timer_action) {
                case SBCCallTimerEvent::Remove:
                    DBG("removing timer %d on call timer request\n", ct_event->timer_id);
                    removeTimer(ct_event->timer_id);
                    return;
                case SBCCallTimerEvent::Set:
                    DBG("setting timer %d to %f on call timer request\n",
                        ct_event->timer_id, ct_event->timeout);
                    setTimer(ct_event->timer_id, ct_event->timeout);
                    return;
                case SBCCallTimerEvent::Reset:
                    DBG("resetting timer %d to %f on call timer request\n",
                        ct_event->timer_id, ct_event->timeout);
                    removeTimer(ct_event->timer_id);
                    setTimer(ct_event->timer_id, ct_event->timeout);
                    return;
                default:
                    ERROR("unknown timer_action in sbc call timer event\n");
                    return;
                }
            case BB_Init:
            case BB_Dialing:
                switch (ct_event->timer_action) {
                case SBCCallTimerEvent::Remove:
                    clearCallTimer(ct_event->timer_id);
                    return;
                case SBCCallTimerEvent::Set:
                case SBCCallTimerEvent::Reset:
                    saveCallTimer(ct_event->timer_id, ct_event->timeout);
                    return;
                default:
                    ERROR("unknown timer_action in sbc call timer event\n");
                    return;
                }
                break;
            default:
                break;
            }
        }
    }

    SBCControlEvent* ctl_event;
    if (ev->event_id == SBCControlEvent_ID &&
        (ctl_event = dynamic_cast<SBCControlEvent*>(ev)) != NULL)
    {
        onControlCmd(ctl_event->cmd, ctl_event->params);
        return;
    }

    CallLeg::process(ev);
}


//////////////////////////////////////////////////////////////////////////////////////////
// was for caller only (SBCDialog)
// FIXME: move the stuff related to CC interface outside of this class?


#define REPLACE_VALS req, app_param, ruri_parser, from_parser, to_parser

void SBCCallLeg::onInvite(const AmSipRequest& req)
{
    DBG("processing initial INVITE %s\n", req.r_uri.c_str());

    ctx.call_profile = &call_profile;
    ctx.app_param = getHeader(req.hdrs, PARAM_HDR, true);

    init();

    modified_req = req;
    aleg_modified_req = req;
    uac_req = req;

    if (!logger &&
        !call_profile.get_logger_path().empty() &&
        (call_profile.log_sip || call_profile.log_rtp))
    {
        // open the logger if not already opened
        ParamReplacerCtx ctx(&call_profile);
        string log_path = ctx.replaceParameters(call_profile.get_logger_path(), "msg_logger_path",req);
        if(!openLogger(log_path)){
            WARN("can't open msg_logger_path: '%s'",log_path.c_str());
        }
    }

    req.log(call_profile.log_sip?getLogger():NULL,
            call_profile.aleg_sensor_level_id&LOG_SIP_MASK?getSensor():NULL);

    uac_ruri.uri = uac_req.r_uri;
    if(!uac_ruri.parse_uri()) {
        DBG("Error parsing R-URI '%s'\n",uac_ruri.uri.c_str());
        throw AmSession::Exception(400,"Failed to parse R-URI");
    }

    call_ctx->cdr->update(req);
    call_ctx->initial_invite = new AmSipRequest(aleg_modified_req);

    if(yeti.config.early_100_trying) {
        msg_logger *logger = getLogger();
        if(logger){
            call_ctx->early_trying_logger->relog(logger);
        }
    } else {
        dlg->reply(req,100,"Connecting");
    }

    if(!radius_auth(this,*call_ctx->cdr,call_profile,req)) {
        processRouting();
    }
}

void SBCCallLeg::onRoutingReady()
{
    call_profile.sst_aleg_enabled = ctx.replaceParameters(
        call_profile.sst_aleg_enabled,
        "enable_aleg_session_timer", aleg_modified_req);

    call_profile.sst_enabled = ctx.replaceParameters(
        call_profile.sst_enabled,
        "enable_session_timer", aleg_modified_req);

    if ((call_profile.sst_aleg_enabled == "yes") ||
        (call_profile.sst_enabled == "yes"))
    {
        call_profile.eval_sst_config(ctx,aleg_modified_req,call_profile.sst_a_cfg);
        if(applySSTCfg(call_profile.sst_a_cfg,&aleg_modified_req) < 0) {
            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
    }

    if (!call_profile.evaluate(ctx, aleg_modified_req)) {
        ERROR("call profile evaluation failed\n");
        throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
    }

    AmUriParser uac_ruri;
    uac_ruri.uri = uac_req.r_uri;
    if(!uac_ruri.parse_uri()) {
        DBG("Error parsing R-URI '%s'\n",uac_ruri.uri.c_str());
        throw AmSession::Exception(400,"Failed to parse R-URI");
    }

    if(call_profile.contact_hiding) {
        if(RegisterDialog::decodeUsername(aleg_modified_req.user,uac_ruri)) {
            uac_req.r_uri = uac_ruri.uri_str();
        }
    } else if(call_profile.reg_caching) {
        // REG-Cache lookup
        uac_req.r_uri = call_profile.retarget(aleg_modified_req.user,*dlg);
    }

    ruri = call_profile.ruri.empty() ? uac_req.r_uri : call_profile.ruri;
    if(!call_profile.ruri_host.empty()) {
        ctx.ruri_parser.uri = ruri;
        if(!ctx.ruri_parser.parse_uri()) {
            WARN("Error parsing R-URI '%s'\n", ruri.c_str());
        } else {
            ctx.ruri_parser.uri_port.clear();
            ctx.ruri_parser.uri_host = call_profile.ruri_host;
            ruri = ctx.ruri_parser.uri_str();
        }
    }
    from = call_profile.from.empty() ? aleg_modified_req.from : call_profile.from;
    to = call_profile.to.empty() ? aleg_modified_req.to : call_profile.to;

    applyAProfile();
    call_profile.apply_a_routing(ctx,aleg_modified_req,*dlg);

    m_state = BB_Dialing;

    // prepare request to relay to the B leg(s)

    if(a_leg && call_profile.keep_vias)
        modified_req.hdrs = modified_req.vias + modified_req.hdrs;

      est_invite_cseq = uac_req.cseq;

    removeHeader(modified_req.hdrs,PARAM_HDR);
    removeHeader(modified_req.hdrs,"P-App-Name");

    if (call_profile.sst_enabled_value) {
        removeHeader(modified_req.hdrs,SIP_HDR_SESSION_EXPIRES);
        removeHeader(modified_req.hdrs,SIP_HDR_MIN_SE);
    }

    size_t start_pos = 0;
    while (start_pos<call_profile.append_headers.length()) {
        int res;
        size_t name_end, val_begin, val_end, hdr_end;
        if ((res = skip_header(call_profile.append_headers, start_pos, name_end, val_begin,
            val_end, hdr_end)) != 0)
        {
            ERROR("skip_header for '%s' pos: %ld, return %d",
                call_profile.append_headers.c_str(),start_pos,res);
            throw AmSession::Exception(500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
        string hdr_name = call_profile.append_headers.substr(start_pos, name_end-start_pos);
        while(!getHeader(modified_req.hdrs,hdr_name).empty()){
            removeHeader(modified_req.hdrs,hdr_name);
        }
        start_pos = hdr_end;
    }

    inplaceHeaderPatternFilter(modified_req.hdrs, call_profile.headerfilter_a2b);

    if (call_profile.append_headers.size() > 2) {
        string append_headers = call_profile.append_headers;
        assertEndCRLF(append_headers);
        modified_req.hdrs+=append_headers;
    }

#undef REPLACE_VALS

    DBG("SBC: connecting to '%s'\n",ruri.c_str());
    DBG("     From:  '%s'\n",from.c_str());
    DBG("     To:  '%s'\n",to.c_str());

    // we evaluated the settings, now we can initialize internals (like RTP relay)
    // we have to use original request (not the altered one) because for example
    // codecs filtered out might be used in direction to caller
    CallLeg::onInvite(aleg_modified_req);

    if (getCallStatus() == Disconnected) {
        // no CC module connected a callee yet
        connectCallee(to, ruri, from, aleg_modified_req, modified_req); // connect to the B leg(s) using modified request
    }
}

void SBCCallLeg::onInviteException(int code,string reason,bool no_reply)
{
    DBG("%s(%p,leg%s) %d:'%s' no_reply = %d",FUNC_NAME,this,a_leg?"A":"B",
        code,reason.c_str(),no_reply);

    if(!call_ctx) return;

    Cdr *cdr = call_ctx->cdr;

    cdr->lock();
    cdr->disconnect_initiator = DisconnectByTS;
    if(cdr->disconnect_internal_code==0){ //update only if not previously was setted
        cdr->disconnect_internal_code = code;
        cdr->disconnect_internal_reason = reason;
    }
    if(!no_reply){
        cdr->disconnect_rewrited_code = code;
        cdr->disconnect_rewrited_reason = reason;
    }
    cdr->unlock();
}

void SBCCallLeg::onEarlyEventException(unsigned int code,const string &reason)
{
    setStopped();
    onInviteException(code,reason,false);
    if(code < 300){
        ERROR("%i is not final code. replace it with 500",code);
        code = 500;
    }
    dlg->reply(uac_req,code,reason);
}

void SBCCallLeg::connectCallee(
    const string& remote_party,
    const string& remote_uri,
    const string &from,
    const AmSipRequest &original_invite,
    const AmSipRequest &invite)
{
    SBCCallLeg* callee_session = SBCFactory::instance()->getCallLegCreator()->create(this);

    callee_session->setLocalParty(from, from);
    callee_session->setRemoteParty(remote_party, remote_uri);

    DBG("Created B2BUA callee leg, From: %s\n", from.c_str());

    // FIXME: inconsistent with other filtering stuff - here goes the INVITE
    // already filtered so need not to be catched (can not) in relayEvent because
    // it is sent other way
    addCallee(callee_session, invite);

    // we could start in SIP relay mode from the beginning if only one B leg, but
    // serial fork might mess it
    // set_sip_relay_only(true);
}

void SBCCallLeg::onCallConnected(const AmSipReply& reply)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    if(call_ctx) {
        Cdr *cdr = call_ctx->cdr;

        if(a_leg) cdr->update(Connect);
        else cdr->update(BlegConnect);

        radius_accounting_start(this,*cdr,call_profile);
    }

    if (a_leg) { // FIXME: really?
        m_state = BB_Connected;
        if (!startCallTimers())
            return;
    }
}

void SBCCallLeg::onStop()
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    if (a_leg && m_state == BB_Connected) { // m_state might be valid for A leg only
        stopCallTimers();
    }

    m_state = BB_Teardown;

    if(call_ctx && a_leg) {
        with_cdr_for_read {
            cdr->update(End);
            cdr_list.erase(cdr);
        }
    }
}

void SBCCallLeg::saveCallTimer(int timer, double timeout)
{
     call_timers[timer] = timeout;
}

void SBCCallLeg::clearCallTimer(int timer)
{
    call_timers.erase(timer);
}

void SBCCallLeg::clearCallTimers()
{
    call_timers.clear();
}

/** @return whether successful */
bool SBCCallLeg::startCallTimers()
{
    for (map<int, double>::iterator it=call_timers.begin();
         it != call_timers.end(); it++)
    {
        DBG("SBC: starting call timer %i of %f seconds\n", it->first, it->second);
        setTimer(it->first, it->second);
    }

    return true;
}

void SBCCallLeg::stopCallTimers() {
    for (map<int, double>::iterator it=call_timers.begin();
         it != call_timers.end(); it++)
    {
        DBG("SBC: removing call timer %i\n", it->first);
        removeTimer(it->first);
    }
}

void SBCCallLeg::onCallStatusChange(const StatusChangeCause &cause)
{
    string reason;

    if(!call_ctx) return;
    SBCCallLeg::CallStatus status = getCallStatus();
    int internal_disconnect_code = 0;

    DBG("Yeti::onStateChange(%p|%s) a_leg = %d",
        this,getLocalTag().c_str(),a_leg);

    switch(status){
    case CallLeg::Ringing: {
        if(!a_leg) {
            if(call_profile.ringing_timeout > 0)
                setTimer(YETI_RINGING_TIMEOUT_TIMER,call_profile.ringing_timeout);
        } else {
            if(call_profile.fake_ringing_timeout)
                removeTimer(YETI_FAKE_RINGING_TIMER);
            if(call_profile.force_one_way_early_media) {
                DBG("force one-way audio for early media (mute legB)");
                AmB2BMedia *m = getMediaSession();
                if(m) {
                    m->mute(false);
                    call_ctx->bleg_early_media_muted = true;
                }
            }
        }
    } break;
    case CallLeg::Connected:
        if(!a_leg) {
            removeTimer(YETI_RINGING_TIMEOUT_TIMER);
        } else {
            if(call_profile.fake_ringing_timeout)
                removeTimer(YETI_FAKE_RINGING_TIMER);
            if(call_ctx->bleg_early_media_muted) {
                AmB2BMedia *m = getMediaSession();
                if(m) m->unmute(false);
            }
        }
        break;
    case CallLeg::Disconnected:
        removeTimer(YETI_RADIUS_INTERIM_TIMER);
        if(a_leg && call_profile.fake_ringing_timeout) {
            removeTimer(YETI_FAKE_RINGING_TIMER);
        }
        break;
    default:
        break;
    }

    switch(cause.reason){
        case CallLeg::StatusChangeCause::SipReply:
            if(cause.param.reply!=NULL){
                reason = "SipReply. code = "+int2str(cause.param.reply->code);
                switch(cause.param.reply->code){
                case 408:
                    internal_disconnect_code = DC_TRANSACTION_TIMEOUT;
                    break;
                case 487:
                    if(call_ctx->isRingingTimeout()){
                        internal_disconnect_code = DC_RINGING_TIMEOUT;
                    }
                    break;
                }
            } else
                reason = "SipReply. empty reply";
            break;
        case CallLeg::StatusChangeCause::SipRequest:
            if(cause.param.request!=NULL){
                reason = "SipRequest. method = "+cause.param.request->method;
            } else
                reason = "SipRequest. empty request";
            break;
        case CallLeg::StatusChangeCause::Canceled:
            reason = "Canceled";
            break;
        case CallLeg::StatusChangeCause::NoAck:
            reason = "NoAck";
            internal_disconnect_code = DC_NO_ACK;
            break;
        case CallLeg::StatusChangeCause::NoPrack:
            reason = "NoPrack";
            internal_disconnect_code = DC_NO_PRACK;
            break;
        case CallLeg::StatusChangeCause::RtpTimeout:
            reason = "RtpTimeout";
            break;
        case CallLeg::StatusChangeCause::SessionTimeout:
            reason = "SessionTimeout";
            internal_disconnect_code = DC_SESSION_TIMEOUT;
            break;
        case CallLeg::StatusChangeCause::InternalError:
            reason = "InternalError";
            internal_disconnect_code = DC_INTERNAL_ERROR;
            break;
        case CallLeg::StatusChangeCause::Other:
            break;
        default:
            reason = "???";
    }

    if(status==CallLeg::Disconnected) {
        with_cdr_for_read {
            if(internal_disconnect_code) {
                unsigned int internal_code,response_code;
                string internal_reason,response_reason;

                CodesTranslator::instance()->translate_db_code(
                    internal_disconnect_code,
                    internal_code,internal_reason,
                    response_code,response_reason,
                    call_ctx->getOverrideId());
                cdr->update_internal_reason(DisconnectByTS,internal_reason,internal_code);
            }
            radius_accounting_stop(this, *cdr);
        }
    }

    DBG("%s(%p,leg%s,state = %s, cause = %s)",FUNC_NAME,this,a_leg?"A":"B",
        callStatus2str(status),
        reason.c_str());
}

void SBCCallLeg::onBLegRefused(AmSipReply& reply)
{
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");
    if(!call_ctx) return;
    Cdr* cdr = call_ctx->cdr;
    CodesTranslator *ct = CodesTranslator::instance();
    unsigned int intermediate_code;
    string intermediate_reason;

    if(!a_leg) return;

    removeTimer(YETI_FAKE_RINGING_TIMER);

    cdr->update(reply);
    cdr->update_bleg_reason(reply.reason,reply.code);

    ct->rewrite_response(reply.code,reply.reason,
        intermediate_code,intermediate_reason,
        call_ctx->getOverrideId(false)); //bleg_override_id
    ct->rewrite_response(intermediate_code,intermediate_reason,
        reply.code,reply.reason,
        call_ctx->getOverrideId(true)); //aleg_override_id
    cdr->update_internal_reason(DisconnectByDST,intermediate_reason,intermediate_code);
    cdr->update_aleg_reason(reply.reason,reply.code);

    if(ct->stop_hunting(reply.code,call_ctx->getOverrideId(false))){
        DBG("stop hunting");
        return;
    }

    DBG("continue hunting");
    //put current resources
    rctl.put(call_ctx->getCurrentProfile()->resource_handler);
    if(call_ctx->initial_invite!=NULL){
        ERROR("%s() intial_invite == NULL",FUNC_NAME);
        return;
    }

    if(chooseNextProfile()) {
        DBG("%s() no new profile, just finish as usual",FUNC_NAME);
        return;
    }

    DBG("%s() has new profile, so create new callee",FUNC_NAME);
    cdr = call_ctx->cdr;

    if(0!=cdr_list.insert(cdr)){
        ERROR("onBLegRefused(): double insert into active calls list. integrity threat");
        ERROR("ctx: attempt = %d, cdr.logger_path = %s",
            call_ctx->attempt_num,cdr->msg_logger_path.c_str());
        return;
    }

    AmSipRequest &req = *call_ctx->initial_invite;
    try {
        connectCallee(req);
    } catch(InternalException &e){
        cdr->update_internal_reason(DisconnectByTS,e.internal_reason,e.internal_code);
        throw AmSession::Exception(e.response_code,e.response_reason);
    }
}

void SBCCallLeg::onCallFailed(CallFailureReason reason, const AmSipReply *reply)
{ }

bool SBCCallLeg::onBeforeRTPRelay(AmRtpPacket* p, sockaddr_storage* remote_addr)
{
    if(rtp_relay_rate_limit.get() && rtp_relay_rate_limit->limit(p->getBufferSize()))
        return false; // drop
    return true; // relay
}

void SBCCallLeg::onAfterRTPRelay(AmRtpPacket* p, sockaddr_storage* remote_addr)
{
    for(list<atomic_int*>::iterator it = rtp_pegs.begin();
        it != rtp_pegs.end(); ++it)
    {
        (*it)->inc(p->getBufferSize());
    }
}

void SBCCallLeg::onRTPStreamDestroy(AmRtpStream *stream) {
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    if(!call_ctx) return;

    with_cdr_for_read {
        if(cdr->writed) return;
        cdr->lock();
        if(a_leg) {
            stream->getPayloadsHistory(cdr->legA_payloads);
            stream->getErrorsStats(cdr->legA_stream_errors);
            cdr->legA_bytes_recvd = stream->getRcvdBytes();
            cdr->legA_bytes_sent = stream->getSentBytes();
        } else {
            stream->getPayloadsHistory(cdr->legB_payloads);
            stream->getErrorsStats(cdr->legB_stream_errors);
            cdr->legB_bytes_recvd = stream->getRcvdBytes();
            cdr->legB_bytes_sent = stream->getSentBytes();
        }
        cdr->unlock();
    }
}

bool SBCCallLeg::reinvite(const AmSdp &sdp, unsigned &request_cseq)
{
    request_cseq = 0;

    AmMimeBody body;
    AmMimeBody *sdp_body = body.addPart(SIP_APPLICATION_SDP);
    if (!sdp_body) return false;

    string body_str;
    sdp.print(body_str);
    sdp_body->parse(SIP_APPLICATION_SDP, (const unsigned char*)body_str.c_str(), body_str.length());

    if (dlg->reinvite("", &body, SIP_FLAGS_VERBATIM) != 0) return false;
    request_cseq = dlg->cseq - 1;
    return true;
}

void SBCCallLeg::holdRequested()
{
    TRACE("%s: hold requested\n", getLocalTag().c_str());
    CallLeg::holdRequested();
}

void SBCCallLeg::holdAccepted()
{
    TRACE("%s: hold accepted\n", getLocalTag().c_str());
    CallLeg::holdAccepted();
}

void SBCCallLeg::holdRejected()
{
    TRACE("%s: hold rejected\n", getLocalTag().c_str());
    CallLeg::holdRejected();
}

void SBCCallLeg::resumeRequested()
{
    TRACE("%s: resume requested\n", getLocalTag().c_str());
    CallLeg::resumeRequested();
}

void SBCCallLeg::resumeAccepted()
{
    TRACE("%s: resume accepted\n", getLocalTag().c_str());
    CallLeg::resumeAccepted();
}

void SBCCallLeg::resumeRejected()
{
    TRACE("%s: resume rejected\n", getLocalTag().c_str());
    CallLeg::resumeRejected();
}

static void replace_address(SdpConnection &c, const string &ip)
{
    if (!c.address.empty()) {
        if (c.addrType == AT_V4) {
            c.address = ip;
            return;
        }
        // TODO: IPv6?
        DBG("unsupported address type for replacing IP");
    }
}

static void alterHoldRequest(AmSdp &sdp, SBCCallProfile::HoldSettings::Activity a, const string &ip)
{
    if (!ip.empty()) replace_address(sdp.conn, ip);
    for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m)
    {
        if (!ip.empty()) replace_address(m->conn, ip);
        m->recv = (a == SBCCallProfile::HoldSettings::sendrecv || a == SBCCallProfile::HoldSettings::recvonly);
        m->send = (a == SBCCallProfile::HoldSettings::sendrecv || a == SBCCallProfile::HoldSettings::sendonly);
    }
}

void SBCCallLeg::alterHoldRequestImpl(AmSdp &sdp)
{
    if (call_profile.hold_settings.mark_zero_connection(a_leg)) {
        static const string zero("0.0.0.0");
        ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), zero);
    } else {
        if (getRtpRelayMode() == RTP_Direct) {
            // we can not put our IP there if not relaying, using empty not to
            // overwrite existing addresses
            static const string empty;
            ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), empty);
        } else {
            // use public IP to be put into connection addresses (overwrite 0.0.0.0
            // there)
            ::alterHoldRequest(sdp, call_profile.hold_settings.activity(a_leg), advertisedIP());
        }
    }
}

void SBCCallLeg::alterHoldRequest(AmSdp &sdp)
{
    TRACE("altering B2B hold request(%s, %s, %s)\n",
        call_profile.hold_settings.alter_b2b(a_leg) ? "alter B2B" : "do not alter B2B",
        call_profile.hold_settings.mark_zero_connection(a_leg) ? "0.0.0.0" : "own IP",
        call_profile.hold_settings.activity_str(a_leg).c_str());

    if (!call_profile.hold_settings.alter_b2b(a_leg)) return;

    alterHoldRequestImpl(sdp);
}

void SBCCallLeg::processLocalRequest(AmSipRequest &req) {
    DBG("%s() local_tag = %s",FUNC_NAME,getLocalTag().c_str());
    updateLocalBody(req.body);
    dlg->reply(req,200,"OK",&req.body,"",SIP_FLAGS_VERBATIM);
}

void SBCCallLeg::createHoldRequest(AmSdp &sdp)
{
    // hack: we need to have other side SDP (if the stream is hold already
    // it should be marked as inactive)
    // FIXME: fix SDP versioning! (remember generated versions and increase the
    // version number in every SDP passing through?)

    AmMimeBody *s = established_body.hasContentType(SIP_APPLICATION_SDP);
    if (s) sdp.parse((const char*)s->getPayload());
    if (sdp.media.empty()) {
        // established SDP is not valid! generate complete fake
        sdp.version = 0;
        sdp.origin.user = "sems";
        sdp.sessionName = "sems";
        sdp.conn.network = NT_IN;
        sdp.conn.addrType = AT_V4;
        sdp.conn.address = "0.0.0.0";

        sdp.media.push_back(SdpMedia());
        SdpMedia &m = sdp.media.back();
        m.type = MT_AUDIO;
        m.transport = TP_RTPAVP;
        m.send = false;
        m.recv = false;
        m.payloads.push_back(SdpPayload(0));
    }

    AmB2BMedia *ms = getMediaSession();
    if (ms) ms->replaceOffer(sdp, a_leg);

    alterHoldRequestImpl(sdp);
}

void SBCCallLeg::setMediaSession(AmB2BMedia *new_session)
{
    if (new_session) {
        if (call_profile.log_rtp) new_session->setRtpLogger(logger);
        else new_session->setRtpLogger(NULL);

        if(a_leg) {
            if(call_profile.aleg_sensor_level_id&LOG_RTP_MASK)
            new_session->setRtpASensor(sensor);
            else new_session->setRtpASensor(NULL);
        } else {
            if(call_profile.bleg_sensor_level_id&LOG_RTP_MASK)
            new_session->setRtpBSensor(sensor);
            else new_session->setRtpBSensor(NULL);
        }
    }
    CallLeg::setMediaSession(new_session);
}

bool SBCCallLeg::openLogger(const std::string &path)
{
    file_msg_logger *log = new pcap_logger();

    if(log->open(path.c_str()) != 0) {
        // open error
        delete log;
        return false;
    }

    // opened successfully
    setLogger(log);
    return true;
}

void SBCCallLeg::setLogger(msg_logger *_logger)
{
    if (logger) dec_ref(logger); // release the old one

    logger = _logger;
    if (logger) inc_ref(logger);

    if (call_profile.log_sip) dlg->setMsgLogger(logger);
    else dlg->setMsgLogger(NULL);

    AmB2BMedia *m = getMediaSession();
    if (m) {
        if (call_profile.log_rtp) m->setRtpLogger(logger);
        else m->setRtpLogger(NULL);
    }
}

void SBCCallLeg::setSensor(msg_sensor *_sensor){
    DBG("SBCCallLeg[%p]: %cleg. change sensor to %p",this,a_leg?'A':'B',_sensor);
    if (sensor) dec_ref(sensor);
    sensor = _sensor;
    if (sensor) inc_ref(sensor);

    if((a_leg && (call_profile.aleg_sensor_level_id&LOG_SIP_MASK)) ||
       (!a_leg && (call_profile.bleg_sensor_level_id&LOG_SIP_MASK)))
    {
        dlg->setMsgSensor(sensor);
    } else {
        dlg->setMsgSensor(NULL);
    }

    AmB2BMedia *m = getMediaSession();
    if(m) {
        if(a_leg) {
            if(call_profile.aleg_sensor_level_id&LOG_RTP_MASK)
            m->setRtpASensor(sensor);
            else m->setRtpASensor(NULL);
        } else {
            if(call_profile.bleg_sensor_level_id&LOG_RTP_MASK)
            m->setRtpBSensor(sensor);
            else m->setRtpBSensor(NULL);
        }
    } else {
        DBG("SBCCallLeg: no media session");
    }
}

void SBCCallLeg::computeRelayMask(const SdpMedia &m, bool &enable, PayloadMask &mask)
{
    if (call_profile.transcoder.isActive()) {
        TRACE("entering transcoder's computeRelayMask(%s)\n", a_leg ? "A leg" : "B leg");

        //SBCCallProfile::TranscoderSettings &transcoder_settings = call_profile.transcoder;
        PayloadMask m1/*, m2*/;
        //bool use_m1 = false;
        /* if "m" contains only "norelay" codecs, relay is enabled for them (main idea
         * of these codecs is to limit network bandwidth and it makes not much sense
         * to transcode between codecs 'which are better to avoid', right?)
         *
         * if "m" contains other codecs, relay is enabled as well
         *
         * => if m contains at least some codecs, relay is enabled */
        enable = !m.payloads.empty();

        /*vector<SdpPayload> &norelay_payloads =
          a_leg ? transcoder_settings.audio_codecs_norelay_aleg : transcoder_settings.audio_codecs_norelay;*/

        vector<SdpPayload>::const_iterator p;
        for (p = m.payloads.begin(); p != m.payloads.end(); ++p) {
            // do not mark telephone-event payload for relay (and do not use it for
            // transcoding as well)
            if(strcasecmp("telephone-event",p->encoding_name.c_str()) == 0) continue;

            // mark every codec for relay in m2
            TRACE("marking payload %d for relay\n", p->payload_type);
            m1.set(p->payload_type);
        }

        /*TRACE("using %s\n", use_m1 ? "m1" : "m2");
        if (use_m1) mask = m1;
        else mask = m2;*/
        if(call_profile.force_relay_CN){
            mask.set(COMFORT_NOISE_PAYLOAD_TYPE);
            TRACE("m1: marking payload 13 (CN) for relay\n");
        }
        mask = m1;
    } else {
        // for non-transcoding modes use default
        CallLeg::computeRelayMask(m, enable, mask);
    }
}

int SBCCallLeg::onSdpCompleted(const AmSdp& local, const AmSdp& remote){
    DBG("%s(%p,leg%s)",FUNC_NAME,this,a_leg?"A":"B");

    AmSdp offer(local),answer(remote);

    const SqlCallProfile *sql_call_profile = call_ctx->getCurrentProfile();
    if(sql_call_profile) {
        cutNoAudioStreams(offer,sql_call_profile->filter_noaudio_streams);
        cutNoAudioStreams(answer,sql_call_profile->filter_noaudio_streams);
    }

    dump_SdpMedia(offer.media,"offer");
    dump_SdpMedia(answer.media,"answer");

    return CallLeg::onSdpCompleted(offer, answer);
}

bool SBCCallLeg::getSdpOffer(AmSdp& offer){
    DBG("%s(%p)",FUNC_NAME,this);

    if(!call_ctx) {
        DBG("getSdpOffer[%s] missed call context",getLocalTag().c_str());
        return CallLeg::getSdpOffer(offer);
    }

    AmB2BMedia *m = getMediaSession();
    if(!m){
        DBG("getSdpOffer[%s] missed media session",getLocalTag().c_str());
        return CallLeg::getSdpOffer(offer);
    }
    if(!m->haveLocalSdp(a_leg)){
        DBG("getSdpOffer[%s] have no local sdp",getLocalTag().c_str());
        return CallLeg::getSdpOffer(offer);
    }

    const AmSdp &local_sdp = m->getLocalSdp(a_leg);
    if(a_leg){
        DBG("use last offer from dialog as offer for legA");
        offer = local_sdp;
    } else {
        DBG("provide saved initial offer for legB");
        offer = call_ctx->bleg_initial_offer;
        m->replaceConnectionAddress(offer,a_leg, localMediaIP(), advertisedIP());
    }
    offer.origin.sessV = local_sdp.origin.sessV+1; //increase session version. rfc4566 5.2 <sess-version>
    return true;
}

void SBCCallLeg::b2bInitial1xx(AmSipReply& reply, bool forward)
{
    if(a_leg) {
        if(reply.code==100) {
            if(call_profile.fake_ringing_timeout)
                setTimer(YETI_FAKE_RINGING_TIMER,call_profile.fake_ringing_timeout);
        } else {
            call_ctx->ringing_sent = true;
        }
    }
    return CallLeg::b2bInitial1xx(reply,forward);
}

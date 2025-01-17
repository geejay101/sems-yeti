#include "yeti.h"
#include "sdp_filter.h"

#include <string.h>
#include <ctime>
#include <cstdio>

#include "log.h"
#include "AmPlugIn.h"
#include "AmArg.h"
#include "jsonArg.h"
#include "AmSession.h"
#include "AmUtils.h"
#include "AmAudioFile.h"
#include "AmMediaProcessor.h"
#include "SDPFilter.h"
#include "CallLeg.h"
#include "Registration.h"
#include "CodecsGroup.h"
#include "Sensors.h"
#include "AmEventDispatcher.h"
#include "ampi/SctpBusAPI.h"
#include "ampi/PostgreSqlAPI.h"
#include "sip/resolver.h"
#include "ObjectsCounter.h"

#include "RedisConnection.h"

#include "cfg/yeti_opts.h"
#include "cfg/cfg_helpers.h"

#include "IPTree.h"

#define EPOLL_MAX_EVENTS 2048

#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_REGISTRAR_KEEPALIVE_INTERVAL 60

#define DEFAULT_REGISTRAR_EXPIRES 1800

#define YETI_SIGNATURE "yeti-switch"
#define YETI_AGENT_SIGNATURE YETI_SIGNATURE " " YETI_VERSION

#define LOG_BUF_SIZE 2048

string yeti_auth_feedback_header("X-Yeti-Auth-Error: ");

void cfg_reader_error(cfg_t *cfg, const char *fmt, va_list ap)
{
    int l = 0;
    char buf[LOG_BUF_SIZE];
    if(cfg->title) {
    //if(cfg->opts->flags & CFGF_TITLE) {
        l = snprintf(buf,LOG_BUF_SIZE,"line:%d section '%s'(%s): ",
            cfg->line,
            cfg->name,
            cfg->title);
    } else {
        l = snprintf(buf,LOG_BUF_SIZE,"line:%d section '%s': ",
            cfg->line,
            cfg->name);
    }
    l+= vsnprintf(buf+l,static_cast<size_t>(LOG_BUF_SIZE-l),fmt,ap);
    ERROR("%.*s",l,buf);
}

Yeti* Yeti::_instance = nullptr;

Yeti *Yeti::create_instance()
{
    if(!_instance)
        _instance = new Yeti();
    return _instance;
}

Yeti& Yeti::instance() {
    return *_instance;
}

void Yeti::cfg_timer_mapping_entry::init_exceptions_counter(const string &key)
{
    exceptions_counter = &stat_group(Counter, MOD_NAME, "config_exceptions").addAtomicCounter()
        .addLabel("type", key);
}

Yeti::Counters::Counters()
  : identity_success(stat_group(Counter,MOD_NAME, "identity_headers_success").addAtomicCounter()),
    identity_failed_parse(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","parse_failed")),
    identity_failed_verify_expired(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","iat_expired")),
    identity_failed_verify_signature(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","wrong_signature")),
    identity_failed_x5u_not_trusted(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","x5u_not_trusted")),
    identity_failed_cert_invalid(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","cert_invalid")),
    identity_failed_cert_not_available(stat_group(Counter,MOD_NAME, "identity_headers_failed").addAtomicCounter()
        .addLabel("reason","cert_not_available"))
{}

Yeti::Yeti()
  : AmEventFdQueue(this)
{
    initCfgTimerMappings();
}

Yeti::~Yeti()
{
    stop(true);

    if(confuse_cfg)
        cfg_free(confuse_cfg);

    CodecsGroups::dispose();
    CodesTranslator::dispose();
    Sensors::dispose();
    Registration::dispose();
}

int Yeti::configure(const std::string& config_buf)
{
    confuse_cfg = cfg_init(yeti_opts, CFGF_NONE);
    if(!confuse_cfg) {
        ERROR("failed to init cfg opts");
        return -1;
    }

    cfg_set_error_function(confuse_cfg,cfg_reader_error);

    switch(cfg_parse_buf(confuse_cfg, config_buf.c_str())) {
    case CFG_SUCCESS:
        break;
    case CFG_PARSE_ERROR:
        ERROR("failed to parse Yeti configuration");
        return -1;
    default:
        ERROR("unexpected error on Yeti configuring");
        return -1;
    }

    if(config.configure(confuse_cfg, cfg))
        return -1;

    cfg_t* identity_sec = cfg_getsec(confuse_cfg, section_name_identity);
    if(identity_sec) {
        if(cert_cache.configure(identity_sec)) {
            ERROR("failed to configure certificates cache for identity verification");
            return -1;
        }
        config.identity_enabled = true;
    } else {
        WARN("missed identity section. Identity validation support will be disabled");
        config.identity_enabled = false;
    }

    return 0;
}

static void apply_yeti_signatures()
{
    if(AmConfig.sdp_origin==DEFAULT_SDP_ORIGIN)
        AmConfig.sdp_origin = YETI_SIGNATURE;

    if(AmConfig.sdp_session_name==DEFAULT_SDP_SESSION_NAME)
        AmConfig.sdp_session_name = YETI_SIGNATURE;

    AmLcConfig::instance().applySignature(YETI_AGENT_SIGNATURE);
}

static void init_counters()
{
    ObjCounterInit(Cdr);
    ObjCounterInit(AuthCdr);
    ObjCounterInit(SqlCallProfile);
}

int Yeti::onLoad()
{
    makeRedisInstance(false);
    start_time = time(nullptr);

    cfg.dump();

    init_rpc();
    init_counters();
    apply_yeti_signatures();

    if((epoll_fd = epoll_create(10)) == -1) {
        ERROR("epoll_create call failed");
        return -1;
    }

    epoll_link(epoll_fd);

    start(); //start yeti thread

    calls_show_limit = static_cast<int>(cfg.getParameterInt("calls_show_limit",100));

    /*if(TrustedHeaders::instance()->configure(cfg)){
        ERROR("TrustedHeaders configure failed");
        return -1;
    }*/

    if (cdr_list.configure(confuse_cfg)) {
        ERROR("CdrList configure failed");
        return -1;
    }

    if(router.configure(confuse_cfg, cfg)) {
        ERROR("SqlRouter configure failed");
        return -1;
    }

    if(configure_filter(&router)){
        ERROR("ActiveCallsFilter configure failed");
        return -1;
    }

    if(init_radius_module()) {
        ERROR("radius module configure failed");
        return -1;
    }

    if(rctl.configure(cfg)){
        ERROR("ResourceControl configure failed");
        return -1;
    }

    if(options_prober_manager.configure()) {
        ERROR("SipProberManager configure failed");
        return -1;
    }

    if(CodecsGroups::instance()->configure(cfg)){
        ERROR("CodecsGroups configure failed");
        return -1;
    }

    if (CodesTranslator::instance()->configure(cfg)){
        ERROR("CodesTranslator configure failed");
        return -1;
    }

    if(Sensors::instance()->configure(cfg)){
    ERROR("Sensors configure failed");
        return -1;
    }

    if(configure_registrar()) {
        ERROR("Failed to configure registrar");
        return -1;
    }

    if(Registration::instance()->configure(cfg)){
        ERROR("Registration agent configure failed");
        return -1;
    }

    if(config.registrar_enabled) {
        registrar_redis.start();
        if(config.registrar_keepalive_interval) {
            keepalive_timer.link(epoll_fd);
            keepalive_timer.set(config.registrar_keepalive_interval,true);
        }
    }

    each_second_timer.link(epoll_fd);
    each_second_timer.set(1e6 /* 1 second */,true);

    db_cfg_reload_timer.link(epoll_fd);
    db_cfg_reload_timer.set(
        std::chrono::duration_cast<std::chrono::microseconds>(config.db_refresh_interval).count(),
        true);

    http_sequencer.setHttpDestinationName(config.http_events_destination);

    //start threads
    router.start();
    rctl.start();
    if(cdr_list.getSnapshotsEnabled())
        cdr_list.start();

    configuration_finished = true;

    onDbCfgReloadTimer();

    return 0;
}

int Yeti::configure_registrar()
{
    config.registrar_enabled = cfg.getParameterInt("registrar_enabled");
    DBG("registrar_enabled: %d", config.registrar_enabled);
    if(!config.registrar_enabled)
        return 0;

    config.registrar_redis_host = cfg.getParameter("registrar_redis_host");
    if(config.registrar_redis_host.empty()) config.registrar_redis_host = DEFAULT_REDIS_HOST;

    config.registrar_redis_port = cfg.getParameterInt("registrar_redis_port");
    if(!config.registrar_redis_port) config.registrar_redis_port = DEFAULT_REDIS_PORT;

    config.registrar_keepalive_interval =
        cfg.getParameterInt("registrar_keepalive_interval", DEFAULT_REGISTRAR_KEEPALIVE_INTERVAL);
    if(config.registrar_keepalive_interval) config.registrar_keepalive_interval =
        config.registrar_keepalive_interval * 1000000;

    config.registrar_expires_min = cfg.getParameterInt("registrar_expires_min");
    DBG("registrar_expires_min: %d", config.registrar_expires_min);

    config.registrar_expires_max = cfg.getParameterInt("registrar_expires_max");
    DBG("registrar_expires_max: %d", config.registrar_expires_max);

    config.registrar_expires_default =
        cfg.getParameterInt("registrar_expires_default", DEFAULT_REGISTRAR_EXPIRES);
    DBG("registrar_expires_default: %d", config.registrar_expires_default);

    if(config.registrar_expires_max && config.registrar_expires_default > config.registrar_expires_max) {
        ERROR("registrar error. default expires %d is gt max value %d",
              config.registrar_expires_default, config.registrar_expires_max);
        return -1;
    }

    if(config.registrar_expires_default < config.registrar_expires_min) {
        ERROR("registrar error. default expires %d is lt min value %d",
              config.registrar_expires_default, config.registrar_expires_min);
        return -1;
    }

    if(0!=registrar_redis.init(
        config.registrar_redis_host,
        config.registrar_redis_port,
        0!=config.registrar_keepalive_interval))
    {
        return -1;
    }

    return 0;
}

void Yeti::run()
{
    int ret, f;
    struct epoll_event events[EPOLL_MAX_EVENTS];

    setThreadName("yeti-worker");
    DBG("start yeti-worker");

    AmEventDispatcher::instance()->addEventQueue(YETI_QUEUE_NAME, this);

    //load configurations from DB without waiting for timer
    //onDbCfgReloadTimer();

    stopped = false;
    do {
        //DBG("epoll_wait...");
        ret = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1);

        //DBG("epoll_wait = %d",ret);

        if(ret == -1 && errno != EINTR){
            ERROR("epoll_wait: %s",strerror(errno));
        }

        if(ret < 1)
            continue;

        for (int n = 0; n < ret; ++n) {
            struct epoll_event &e = events[n];
            f = e.data.fd;

            if(f==keepalive_timer){
                registrar_redis.on_keepalive_timer();
                keepalive_timer.read();
            } else if(f==db_cfg_reload_timer) {
                onDbCfgReloadTimer();
                db_cfg_reload_timer.read();
            } else if(f==each_second_timer) {
                const auto now(std::chrono::system_clock::now());
                if(config.identity_enabled)
                    cert_cache.onTimer(now);
                each_second_timer.read();
            } else if(f == -queue_fd()) {
                clear_pending();
                processEvents();
            }
        }
    } while(!stopped);

    AmEventDispatcher::instance()->delEventQueue(YETI_QUEUE_NAME);

    INFO("yeti-worker finished");
}

void Yeti::on_stop()
{
    uint64_t u = 1;

    DBG("Yeti::on_stop");

    cdr_list.stop();
    rctl.stop();
    router.stop();
    registrar_redis.stop();

    stopped = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    ::write(queue_fd(), &u, sizeof(uint64_t)); //trigger events processing
#pragma GCC diagnostic pop
}

#define ON_EVENT_TYPE(type) if(type *e = dynamic_cast<type *>(ev))

void Yeti::process(AmEvent *ev)
{
    ON_EVENT_TYPE(RedisReplyEvent) {
        /*DBG("got RedisReplyEvent id = %d data:\n%s",
            e->user_type_id,
            AmArg::print(e->data).c_str());*/
        switch(e->user_type_id) {
        case YETI_REDIS_REGISTER_TYPE_ID:
            processRedisRegisterReply(*e);
            break;
        case YETI_REDIS_RPC_AOR_LOOKUP_TYPE_ID:
            processRedisRpcAorLookupReply(*e);
            break;
        }
    } else
    ON_EVENT_TYPE(HttpPostResponseEvent) {
        http_sequencer.processHttpReply(*e);
    } else
    ON_EVENT_TYPE(HttpGetResponseEvent) {
        cert_cache.processHttpReply(*e);
    } else
    ON_EVENT_TYPE(PGResponse) {
        if(configuration_finished) {
            if(e->token == "check_states") {
                onDbCfgReloadTimerResponse(*e);
            } else {
                auto it = db_config_timer_mappings.find(e->token);
                if(it != db_config_timer_mappings.end()) {
                    bool exception = true;
                    try {
                        DBG("call on_db_response() for '%s'", e->token.data());
                        it->second.on_db_response(*e);
                        exception = false;
                    } catch(AmArg::OutOfBoundsException &) {
                        ERROR("AmArg::OutOfBoundsException in cfg timer handler: %s",
                            e->token.data());
                    } catch(AmArg::TypeMismatchException &) {
                        ERROR("AmArg::TypeMismatchException in cfg timer handler: %s",
                            e->token.data());
                    } catch(std::exception &exception) {
                        ERROR("std::exception in cfg timer handler '%s': %s",
                            e->token.data(), exception.what());
                    } catch(std::string &s) {
                        ERROR("cfg timer handler %s exception: %s",
                            e->token.data(), s.data());
                    } catch(...) {
                        ERROR("exception in cfg timer handler: %s", e->token.data());
                    }

                    if(exception) {
                        it->second.exceptions_counter->inc();
                    }
                } else {
                    ERROR("unknown db response token: %s", e->token.data());
                }
            }
        } else {
            sync_db.db_reply_token = e->token;
            sync_db.db_reply_result = e->result;
            sync_db.db_reply_condition.set(sync_db::DB_REPLY_RESULT);
        }
    } else
    ON_EVENT_TYPE(PGResponseError) {
        ERROR("got PGResponseError '%s' for token: %s",
            e->error.data(), e->token.data());
        if(configuration_finished) {
            //pass
        } else {
            sync_db.db_reply_token = e->token;
            sync_db.db_reply_condition.set(sync_db::DB_REPLY_ERROR);
        }
    } else
    ON_EVENT_TYPE(PGTimeout) {
        ERROR("got PGTimeout for token: %s", e->token.data());
        if(configuration_finished) {
            //pass
        } else {
            sync_db.db_reply_token = e->token;
            sync_db.db_reply_condition.set(sync_db::DB_REPLY_TIMEOUT);
        }
    } else 
    ON_EVENT_TYPE(YetiComponentInited) {
        component_inited[e->type] = true;
    } else
    ON_EVENT_TYPE(AmSystemEvent) {
        if(e->sys_event==AmSystemEvent::ServerShutdown) {
            DBG("got shutdown event");
            stop();
        }
        return;
    } else
        DBG("got unknown event %s", typeid(*ev).name());
}

void Yeti::processRedisRegisterReply(RedisReplyEvent &e)
{
    static string contact_hdr = SIP_HDR_COLSP(SIP_HDR_CONTACT);
    static string expires_param_prefix = ";expires=";

    const AmSipRequest &req = *dynamic_cast<AmSipRequest *>(e.user_data.get());
    //DBG("e.data: %s",AmArg::print(e.data).c_str());

    if(RedisReplyEvent::SuccessReply!=e.result) {
        ERROR("error reply from redis %s. for request from %s:%hu",
              AmArg::print(e.data).c_str(),
              req.remote_ip.data(), req.remote_port);
        AmSipDialog::reply_error(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        return;
    }

    if(isArgUndef(e.data)) {
        DBG("nil reply from redis. no bindings");
        AmSipDialog::reply_error(req, 200, "OK");
        return;
    }

    /* response layout:
     * [
     *   [ contact1 , expires1, contact_key1, path1, interface_id1 ]
     *   [ contact2 , expires2, contact_key2, path2, interface_id2 ]
     *   ...
     * ]
     */

    if(!isArgArray(e.data)) {
        ERROR("error/unexpected reply from redis: %s for request from %s:%hu. Contact:'%s'",
              AmArg::print(e.data).c_str(),
              req.remote_ip.data(), req.remote_port,
              req.contact.data());
        if(e.data.is<AmArg::CStr>()) {
            AmSipDialog::reply_error(req, 500, e.data.asCStr());
        } else {
            AmSipDialog::reply_error(req, 500, SIP_REPLY_SERVER_INTERNAL_ERROR);
        }
        return;
    }

    string hdrs;
    int n = static_cast<int>(e.data.size());
    for(int i = 0; i < n; i++) {
        AmArg &d = e.data[i];
        if(!isArgArray(d) || d.size()!=5) {
            ERROR("unexpected AoR layout in reply from redis: %s. skip it",AmArg::print(d).c_str());
            continue;
        }
        AmArg &contact_arg = d[0];
        if(!isArgCStr(contact_arg)) {
            ERROR("unexpected contact variable type from redis. skip it");
            continue;
        }
        string contact = contact_arg.asCStr();
        if(contact.empty()) {
            ERROR("empty contact in reply from redis. skip it");
            continue;
        }

        AmArg &expires_arg = d[1];
        if(!isArgLongLong(expires_arg)) {
            ERROR("unexpected expires value in redis reply: %s, skip it",AmArg::print(expires_arg).c_str());
            continue;
        }

        AmUriParser c;
        c.uri = contact;
        if(!c.parse_uri()) {
            ERROR("failed to parse contact uri: %s, skip it",contact.c_str());
            continue;
        }

        hdrs+=contact_hdr + c.print();
        hdrs+=expires_param_prefix+longlong2str(expires_arg.asLongLong());
        hdrs+=CRLF;

        //update KeepAliveContexts
        if(config.registrar_keepalive_interval!=0) {
            registrar_redis.updateKeepAliveContext(
                d[2].asCStr(),  //key
                contact,        //aor
                d[3].asCStr(),  //path
                arg2int(d[4])   //interface_id
            );
        }
    }

    AmSipDialog::reply_error(req, 200, "OK", hdrs);
}

void Yeti::processRedisRpcAorLookupReply(RedisReplyEvent &e)
{
    DBG("processRedisRpcAorLookupReply");
    auto &ctx = *dynamic_cast<RegistrarRedisConnection::RpcAorLookupCtx *>(e.user_data.release());
    ctx.data = e.data;
    ctx.result = e.result;
    DBG("ctx.cond: %p",&ctx.cond);
    ctx.cond.set(true);
}

bool Yeti::isAllComponentsInited()
{
    for(int i = 0; i < YetiComponentInited::MaxType; i++) {
        if(!component_inited[i]) return false;
    }
    return true;
}

void Yeti::initCfgTimerMappings()
{
    db_config_timer_mappings = {
        //cert_cache
        { "stir_shaken_trusted_certificates", {
            [&](const string &key) {
                if(!config.identity_enabled)
                    return;
                yeti_routing_db_query(
                    "SELECT * FROM load_stir_shaken_trusted_certificates()", key);
            },
            [&](const PGResponse &e) {
                cert_cache.reloadTrustedCertificates(e.result);
            }}
        },
        { "stir_shaken_trusted_repositories", {
            [&](const string &key) {
                if(!config.identity_enabled)
                    return;
                yeti_routing_db_query(
                    "SELECT * FROM load_stir_shaken_trusted_repositories()", key);
            },
            [&](const PGResponse &e) {
                cert_cache.reloadTrustedRepositories(e.result);
            }}
        },
        { "stir_shaken_signing_certificates", {
            [&](const string &key) {
                if(!config.identity_enabled)
                    return;
                yeti_routing_db_query(
                    "SELECT * FROM load_stir_shaken_signing_certificates()", key);
            },
            [&](const PGResponse &e) {
                cert_cache.reloadSigningKeys(e.result);
            }}
        },

        //orig_pre_auth
        { "ip_auth", {
            [&](const string &key) {
                auto query = new PGParamExecute(
                  PGQueryData(
                      yeti_routing_pg_worker,
                      "SELECT * FROM load_ip_auth($1,$2)",
                      true, /* single */
                      YETI_QUEUE_NAME,
                      key),
                  PGTransactionData(), false);
                query->addParam(AmConfig.node_id).addParam(config.pop_id);
                AmEventDispatcher::instance()->post(POSTGRESQL_QUEUE, query);
            },
            [&](const PGResponse &e) {
                orig_pre_auth.reloadLoadIPAuth(e.result);
            }}
        },
        { "trusted_lb", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * FROM load_trusted_lb()", key);
            },
            [&](const PGResponse &e) {
                orig_pre_auth.reloadLoadBalancers(e.result);
            }}
        },

        //Sensors
        { "sensors", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * FROM load_sensor()", key);
            },
            [&](const PGResponse &e) {
                Sensors::instance()->load_sensors_config(e.result);
            }}
        },

        /* CodesTranslator performs 6 SQL queries to load data.
         * use artifical subkeys to distinguish them */
        { "translations", {
            [&](const string &) {
                //iterate subkeys
                auto end_it = db_config_timer_mappings.lower_bound("translations/");
                for(auto it = db_config_timer_mappings.upper_bound("translations.");
                    it != end_it; ++it)
                {
                    it->second.on_reload(it->first);
                }
            },
            [&](const PGResponse &) {
                //never called. alias key
            }}
        },
        { "translations.dc_rerouting", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * FROM load_disconnect_code_rerouting()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_rerouting(e.result);
            }
        }},
        { "translations.dc_rewrite", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * FROM load_disconnect_code_rewrite()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_rewrite(e.result);
            }
        }},
        { "translations.dc_refuse", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * from load_disconnect_code_refuse()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_refuse(e.result);
            }
        }},
        { "translations.dc_refuse_override", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * from load_disconnect_code_refuse_overrides()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_refuse_overrides(e.result);
            }
        }},
        { "translations.dc_rerouting_override", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * from load_disconnect_code_rerouting_overrides()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_rerouting_overrides(e.result);
            }
        }},
        { "translations.dc_rewrite_override", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * from load_disconnect_code_rewrite_overrides()", key);
            },
            [&](const PGResponse &e) {
                CodesTranslator::instance()->load_disconnect_code_rewrite_overrides(e.result);
            }
        }},

        //CodecsGroups
        { "codec_groups", {
            [&](const string &key) {
                yeti_routing_db_query(
                    "SELECT * from load_codecs()", key);
            },
            [&](const PGResponse &e) {
                CodecsGroups::instance()->load_codecs(e.result);
            }
        }},

        //Registration
        { "registrations", {
            [&](const string &key) {
                auto query = new PGParamExecute(
                  PGQueryData(
                      yeti_routing_pg_worker,
                      "SELECT * FROM load_registrations_out($1,$2)",
                      true, /* single */
                      YETI_QUEUE_NAME,
                      key),
                  PGTransactionData(), false);
                query->addParam(config.pop_id).addParam(AmConfig.node_id);
                AmEventDispatcher::instance()->post(POSTGRESQL_QUEUE, query);
            },
            [&](const PGResponse &e) {
                Registration::instance()->load_registrations(e.result);
            }}
        },

        //YetiRadius
        { "radius_authorization_profiles", {
            [&](const string &key) {
                if(config.use_radius)
                    yeti_routing_db_query("SELECT * from load_radius_profiles()", key);
            },
            [&](const PGResponse &e) {
                load_radius_auth_connections(e.result);
            }}
        },
        { "radius_accounting_profiles", {
            [&](const string &key) {
                if(config.use_radius)
                    yeti_routing_db_query("SELECT * from load_radius_accounting_profiles()", key);
            },
            [&](const PGResponse &e) {
                load_radius_acc_connections(e.result);
            }}
        },

        //Auth
        { "auth_credentials", {
            [&](const string &key) {
                yeti_routing_db_query("SELECT * from load_incoming_auth()", key);
            },
            [&](const PGResponse &e) {
                router.reload_credentials(e.result);
            }}
        },

        //OptionsProberManager
        { "options_probers", {
            [&](const string &key) {
                auto query = new PGParamExecute(
                PGQueryData(
                    yeti_routing_pg_worker,
                    "SELECT * FROM load_sip_options_probers($1)",
                    true, /* single */
                    YETI_QUEUE_NAME,
                    key),
                PGTransactionData(), false);
                query->addParam(config.pop_id).addParam(AmConfig.node_id);
                AmEventDispatcher::instance()->post(POSTGRESQL_QUEUE, query);
            },
            [&](const PGResponse &e) {
                options_prober_manager.load_probers(e.result);
            }}
        },
    };

    for(auto &mapping: db_config_timer_mappings)
        mapping.second.init_exceptions_counter(mapping.first);
}

void Yeti::onDbCfgReloadTimer() noexcept
{
    yeti_routing_db_query("SELECT * FROM check_states()", "check_states");
}

void Yeti::onDbCfgReloadTimerResponse(const PGResponse &e) noexcept
{
    //DBG("onDbCfgReloadTimerResponse");
    try {
        const AmArg &r = e.result[0];
        for(auto &a : r) {
            //DBG("%s: %d",a.first.data(),a.second.asInt());
            if(!db_cfg_states.hasMember(a.first) ||
               a.second.asInt() > db_cfg_states[a.first].asInt())
            {
                DBG("new or newer db_state %d for: %s",
                    a.second.asInt(), a.first.data());
                auto it = db_config_timer_mappings.find(a.first);
                if(it != db_config_timer_mappings.end()) {
                    it->second.on_reload(it->first);
                } else {
                    ERROR("unknown db_state: %s", a.first.data());
                }
            }
        }
        db_cfg_states = r;
    } catch(...) {
        DBG("exception on CfgReloadTimer response processing");
    }
}

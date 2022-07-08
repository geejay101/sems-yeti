#pragma once

#include "SBCCallProfile.h"
#include "db/PgConnectionPool.h"
#include "AmUtils.h"
#include "HeaderFilter.h"
#include <algorithm>
#include "db/DbTypes.h"
#include "cdr/CdrBase.h"
#include "cdr/AuthCdr.h"
#include "CodesTranslator.h"
#include "UsedHeaderField.h"
#include "Auth.h"
#include "CallCtx.h"

#include "RedisConnection.h"

using std::string;
using std::list;
using std::vector;

string const getprofile_sql_statement_name("getprofile");

struct GetProfileException {
    int code;
    bool fatal; //if true we should reload pg connection
    GetProfileException(int c, bool h)
      : code(c),
        fatal(h)
    {}
};

class UsageCounterHelper {
    AtomicCounter &counter;
  public:
    UsageCounterHelper(AtomicCounter &counter)
      : counter(counter)
    {
        counter.inc();
    }
    ~UsageCounterHelper()
    {
        counter.dec();
    }
};

class SqlRouter
  : public Auth
{
    //stats
    AtomicCounter &db_hits, &db_hits_time, &hits, &active_requests;
    double gt_min,gt_max;
    double gps_max,gps_avg;
    time_t mi_start;
    time_t mi;
    unsigned int gpi;

    //CdrWriter *cdr_writer;

    vector<UsedHeaderField> used_header_fields;
    int failover_to_slave;
    string writecdr_schema;
    string writecdr_function;
    string authlog_function;
    string routing_schema;
    string routing_function;
    PreparedQueryArgs auth_log_types, getprofile_types;
    //PreparedQueriesT prepared_queries;
    //PreparedQueriesT cdr_prepared_queries;
    DynFieldsT dyn_fields;

    int load_db_interface_in_out();

    AmArg db_async_get_profiles(
      const std::string &local_tag,
      const AmSipRequest&,
      Auth::auth_id_type auth_id,
      AmArg *identity_data);

    void dbg_get_profiles(AmArg &fields_values);
    void update_counters(struct timeval &start_time);

  public:
    SqlRouter();
    ~SqlRouter();

    int configure(cfg_t *confuse_cfg, AmConfigReader &cfg);

    void getprofiles(
        const std::string &local_tag,
        const AmSipRequest&,
        CallCtx &ctx,
        Auth::auth_id_type auth_id,
        AmArg *identity_data);

    int start();
    void stop();

    void align_cdr(Cdr &cdr);
    void write_cdr(Cdr *cdr, bool last);
    void write_auth_log(const AuthCdr &auth_log);

    void log_auth(
        const AmSipRequest& req,
        bool success,
        AmArg &ret,
        Auth::auth_id_type auth_id = 0);

    void send_and_log_auth_challenge(
        const AmSipRequest &req, const string &internal_reason,
        const string &hdrs, bool post_auth_log);

    void dump_config();
    void closeCdrFiles();
    void getStats(AmArg &arg);
    void getConfig(AmArg &arg);
    void showOpenedFiles(AmArg &arg);
    void showRetryQueues(AmArg &arg);

    const DynFieldsT &getDynFields() const { return dyn_fields; }

    /*! return true if call refused */
    bool check_and_refuse(
        SqlCallProfile *profile,Cdr *cdr,
        const AmSipRequest& req,ParamReplacerCtx& ctx,
        bool send_reply = false);
};

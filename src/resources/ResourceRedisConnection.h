#ifndef RESOURCE_REDIS_CONNECTION_H
#define RESOURCE_REDIS_CONNECTION_H

#include "ResourceSequences.h"

extern const string RESOURCE_QUEUE_NAME;

struct RedisConfig {
    short port;
    string server;
    int timeout;
};

enum ResourceResponse {
	RES_SUCC,			//we successful got all resources
	RES_BUSY,			//one of resources is busy
	RES_ERR				//error occured on interaction with cache
};

class ResourceRedisConnection : public RedisConnectionPool
{
    RedisConfig writecfg;
    RedisConfig readcfg;

    RedisConnection* write_async;
    RedisConnection* read_async;

    InvalidateResources inv_seq;
    AmCondition<bool> resources_inited;
    ResourceOperationList res_queue;
    OperationResources* op_seq;
protected:
    int cfg2RedisCfg(const AmConfigReader &cfg, RedisConfig &rcfg,string prefix);
    bool is_ready();
    void queue_op();
    void operate(ResourceOperationList& rol);

    void on_connect(RedisConnection* c) override;
    void on_disconnect(RedisConnection* c) override;

    void get_resource_state(const JsonRpcRequestEvent& req);
    void get(ResourceList &rl);
public:
    ResourceRedisConnection(const string& queue_name = RESOURCE_QUEUE_NAME);
    ~ResourceRedisConnection();

    int configure(const AmConfigReader &cfg);
    int init();
    bool invalidate_resources();
    void get_config(AmArg& ret);

    void process(AmEvent* event) override;
    void process_jsonrpc_request(const JsonRpcRequestEvent& event);
    void process_reply_event(RedisReplyEvent &event) override;

    typedef void cb_func(void);
    cb_func *resources_initialized_cb;
    void registerResourcesInitializedCallback(cb_func *func);

    void put(ResourceList &rl);
    ResourceResponse get(ResourceList &rl, ResourceList::iterator &resource);

    bool get_resource_state(const string& connection_id,
                            const AmArg& request_id,
                            const AmArg& params);

    RedisConnection* get_write_conn(){ return write_async; }
    RedisConnection* get_read_conn(){ return read_async; }
};

#endif/*RESOURCE_REDIS_CONNECTION_H*/

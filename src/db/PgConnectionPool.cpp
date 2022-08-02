#include "PgConnectionPool.h"

int PgConnectionPoolCfg::cfg2PgCfg(AmConfigReader& cfg)
{
    dbconfig.cfg2dbcfg(cfg,name);

    size = cfg.getParameterInt(name+"_pool_size",10);
    max_exceptions = cfg.getParameterInt(name+"_max_exceptions",0);
    check_interval=cfg.getParameterInt(name+"_check_interval",25);
    max_wait=cfg.getParameterInt(name+"_max_wait",125);
    statement_timeout=cfg.getParameterInt(name+"_statement_timeout",0);
    routing_init_function = cfg.getParameter("routing_init_function");

    return 0;
}

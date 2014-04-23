/****************************************************************************
 *
 * Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2005-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 ****************************************************************************/

#include "stream_icmp.h"
#include "icmp_config.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "snort_types.h"
#include "snort_debug.h"
#include "decode.h"
#include "mstring.h"
#include "sfxhash.h"
#include "util.h"
#include "stream_common.h"
#include "flow/flow.h"
#include "flow/flow_control.h"
#include "flow/session.h"

#include "stream_tcp.h"
#include "stream_udp.h"

#include "parser.h"
#include "perf_monitor/perf.h"

#include "profiler.h"
#ifdef PERF_PROFILING
static THREAD_LOCAL PreprocStats s5IcmpPerfStats;

static PreprocStats* icmp_get_profile(const char* key)
{
    if ( !strcmp(key, "icmp") )
        return &s5IcmpPerfStats;

    return nullptr;
}
#endif

static SessionStats gicmpStats;
static THREAD_LOCAL SessionStats icmpStats;

class IcmpSession : public Session
{
public:
    IcmpSession(Flow*);

    void* get_policy(void*, Packet*);
    bool setup(Packet*);
    void update_direction(char dir, snort_ip*, uint16_t port);
    int process(Packet*);
    void clear();

public:
    uint32_t echo_count;
    struct timeval ssn_time;
};

Session* get_icmp_session(Flow* lws)
{
    return new IcmpSession(lws);
}

Stream5IcmpConfig::Stream5IcmpConfig()
{
    session_timeout = 30;
}

//------------------------------------------------------------------------
// private functions
//------------------------------------------------------------------------

static void Stream5PrintIcmpConfig(Stream5IcmpConfig* pc)
{
    LogMessage("Stream5 ICMP config:\n");
    LogMessage("    Timeout: %d seconds\n", pc->session_timeout);
}

static void IcmpSessionCleanup(Flow *ssn)
{
    if (ssn->s5_state.session_flags & SSNFLAG_PRUNED)
    {
        CloseStreamSession(&sfBase, SESSION_CLOSED_PRUNED);
    }
    else if (ssn->s5_state.session_flags & SSNFLAG_TIMEDOUT)
    {
        CloseStreamSession(&sfBase, SESSION_CLOSED_TIMEDOUT);
    }
    else
    {
        CloseStreamSession(&sfBase, SESSION_CLOSED_NORMALLY);
    }

    ssn->clear();

    icmpStats.released++;
}

static int ProcessIcmpUnreach(Packet *p)
{
    /* Handle ICMP unreachable */
    FlowKey skey;
    Flow *ssn = NULL;
    uint16_t sport;
    uint16_t dport;
    sfip_t *src;
    sfip_t *dst;

    /* No "orig" IP Header */
    if (!p->orig_iph)
        return 0;

    /* Get TCP/UDP/ICMP session from original protocol/port info
     * embedded in the ICMP Unreach message.  This is already decoded
     * in p->orig_foo.  TCP/UDP ports are decoded as p->orig_sp/dp.
     */
    skey.protocol = GET_ORIG_IPH_PROTO(p);
    sport = p->orig_sp;
    dport = p->orig_dp;

    src = GET_ORIG_SRC(p);
    dst = GET_ORIG_DST(p);

    if (sfip_fast_lt6(src, dst))
    {
        COPY4(skey.ip_l, src->ip32);
        skey.port_l = sport;
        COPY4(skey.ip_h, dst->ip32);
        skey.port_h = dport;
    }
    else if (IP_EQUALITY(GET_ORIG_SRC(p), GET_ORIG_DST(p)))
    {
        COPY4(skey.ip_l, src->ip32);
        COPY4(skey.ip_h, skey.ip_l);
        if (sport < dport)
        {
            skey.port_l = sport;
            skey.port_h = dport;
        }
        else
        {
            skey.port_l = dport;
            skey.port_h = sport;
        }
    }
    else
    {
        COPY4(skey.ip_l, dst->ip32);
        COPY4(skey.ip_h, src->ip32);
        skey.port_l = dport;
        skey.port_h = sport;
    }

    if (p->vh)
        skey.vlan_tag = (uint16_t)VTH_VLAN(p->vh);
    else
        skey.vlan_tag = 0;

    switch (skey.protocol)
    {
    case IPPROTO_TCP:
        /* Lookup a TCP session */
        ssn = Stream::get_session(&skey);
        break;
    case IPPROTO_UDP:
        /* Lookup a UDP session */
        ssn = Stream::get_session(&skey);
        break;
    case IPPROTO_ICMP:
        /* Lookup a ICMP session */
        ssn = Stream::get_session(&skey);
        break;
    }

    if (ssn)
    {
        /* Mark this session as dead. */
        DEBUG_WRAP(DebugMessage(DEBUG_STREAM_STATE,
            "Marking session as dead, per ICMP Unreachable!\n"););
        ssn->s5_state.session_flags |= SSNFLAG_DROP_CLIENT;
        ssn->s5_state.session_flags |= SSNFLAG_DROP_SERVER;
        ssn->session_state |= STREAM5_STATE_UNREACH;
    }

    return 0;
}

//------------------------------------------------------------------------
// public functions
//------------------------------------------------------------------------

Stream5IcmpConfig* Stream5ConfigIcmp(SnortConfig*, char*)
{
    RegisterPreprocessorProfile(
        "icmp", &s5IcmpPerfStats, 0, &totalPerfStats, icmp_get_profile);

    Stream5IcmpConfig* icmp_config = 
        (Stream5IcmpConfig*)SnortAlloc(sizeof(*icmp_config));

    return icmp_config;
}

void Stream5IcmpConfigFree(Stream5IcmpConfig *config)
{
    if (config == NULL)
        return;

    free(config);
}

int Stream5VerifyIcmpConfig(SnortConfig*, Stream5IcmpConfig* config)
{
    if (config == NULL)
        return -1;

    return 0;
}

//-------------------------------------------------------------------------
// IcmpSession methods
//-------------------------------------------------------------------------

IcmpSession::IcmpSession(Flow* flow) : Session(flow)
{
    setup(nullptr);
}

bool IcmpSession::setup(Packet*)
{
    echo_count = 0;
    ssn_time.tv_sec = 0;
    ssn_time.tv_usec = 0;
    return true;
}

void IcmpSession::clear()
{
    IcmpSessionCleanup(flow);
}

void* IcmpSession::get_policy(void* pv, Packet*)
{
    return pv;
}

int IcmpSession::process(Packet* p)
{
    int status;

    switch (p->icmph->type)
    {
    case ICMP_DEST_UNREACH:
        status = ProcessIcmpUnreach(p);
        break;

    default:
        /* We only handle the above ICMP messages with stream5 */
        status = 0;
        break;
    }

    return status;
}

#define icmp_sender_ip flow->client_ip
#define icmp_responder_ip flow->server_ip

void IcmpSession::update_direction(char dir, snort_ip* ip, uint16_t)
{
    if (IP_EQUALITY(&icmp_sender_ip, ip))
    {
        if ((dir == SSN_DIR_SENDER) && (flow->s5_state.direction == SSN_DIR_SENDER))
        {
            /* Direction already set as SENDER */
            return;
        }
    }
    else if (IP_EQUALITY(&icmp_responder_ip, ip))
    {
        if ((dir == SSN_DIR_RESPONDER) && (flow->s5_state.direction == SSN_DIR_RESPONDER))
        {
            /* Direction already set as RESPONDER */
            return;
        }
    }

    /* Swap them -- leave ssn->s5_state.direction the same */
    snort_ip tmpIp = icmp_sender_ip;
    icmp_sender_ip = icmp_responder_ip;
    icmp_responder_ip = tmpIp;
}

void icmp_show(Stream5IcmpConfig* icmp_config)
{
    Stream5PrintIcmpConfig(icmp_config);
}

void icmp_sum()
{
    sum_stats((PegCount*)&gicmpStats, (PegCount*)&icmpStats,
        session_peg_count);
}

void icmp_stats()
{
    // FIXIT need to get these before delete flow_con
    //flow_con->get_prunes(IPPROTO_UDP, icmpStats.prunes);

    show_stats((PegCount*)&gicmpStats, session_pegs, session_peg_count,
        "stream5_icmp");
}

void icmp_reset_stats()
{
    memset(&icmpStats, 0, sizeof(icmpStats));
    flow_con->reset_prunes(IPPROTO_ICMP);
}


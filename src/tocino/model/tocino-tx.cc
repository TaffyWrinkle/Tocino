/* -*- Mode:C++; c-file-style:"microsoft"; indent-tabs-mode:nil; -*- */

#include <cstdio>
#include <string>

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/data-rate.h"

#include "callback-queue.h"
#include "tocino-arbiter.h"
#include "tocino-channel.h"
#include "tocino-flit-id-tag.h"
#include "tocino-misc.h"
#include "tocino-net-device.h"
#include "tocino-rx.h"
#include "tocino-simple-arbiter.h"
#include "tocino-tx.h"

NS_LOG_COMPONENT_DEFINE ("TocinoTx");

#ifdef NS_LOG_APPEND_CONTEXT
#pragma push_macro("NS_LOG_APPEND_CONTEXT")
#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT \
    { std::clog << "(" \
                << (int) m_tnd->GetTocinoAddress().GetX() << "," \
                << (int) m_tnd->GetTocinoAddress().GetY() << "," \
                << (int) m_tnd->GetTocinoAddress().GetZ() << ") " \
                << m_portNumber << " "; }
#endif

namespace ns3 {

TocinoTx::TocinoTx( const uint32_t portNumber, Ptr<TocinoNetDevice> tnd, Ptr<TocinoArbiter> arbiter )
    : m_portNumber( portNumber )
    , m_xState( TocinoAllXON )
    , m_remoteXState( TocinoAllXON )
    , m_doUpdateXState( 0 )
    , m_state( IDLE )
    , m_tnd( tnd )
    , m_queues( tnd->GetNQueues() )
    , m_channel( NULL )
    , m_arbiter( arbiter )
{}

TocinoTx::~TocinoTx()
{}

void
TocinoTx::SetXState( const TocinoFlowControlState& newXState )
{
    NS_LOG_LOGIC( "set local XState to " << newXState.to_string() );

    // If we are resuming any VCs, we should kick transmit
    bool shouldTransmit = ( ~m_xState & newXState ).any();

    m_xState = newXState;

    if( shouldTransmit )
    {
        Transmit();
    }
}

uint32_t
TocinoTx::GetPortNumber() const
{
    return m_portNumber;
}

bool
TocinoTx::IsVCPaused(const uint32_t vc)
{
    TocinoVCBitSet xoffBits = ~m_xState;
    return xoffBits[vc];
}

bool
TocinoTx::IsAnyVCPaused() const
{
    TocinoVCBitSet xoffBits = ~m_xState;
    return xoffBits.any();
}

void TocinoTx::SetChannel(Ptr<TocinoChannel> channel)
{
    m_channel = channel;
}

void TocinoTx::RemotePause( const uint8_t vc )
{
    // FIXME: cast required to actually print?!
    NS_LOG_FUNCTION( static_cast<unsigned>(vc) );

    // Asserted bits in m_remoteXState indicate XON.
    // Pausing can ONLY clear bits in m_remoteXState
    
    const TocinoVCBitSet vcMask = ( 1 << vc );
   
    const TocinoVCBitSet unsentResumes = ( m_remoteXState & m_doUpdateXState );

    if( ( unsentResumes & vcMask ).any() )
    {
        // We had an unsent resume on this VC

        // Simply clear remoteXState bit to indicate XOFF
        m_remoteXState &= ~vcMask;

        // No longer need to update this VC
        m_doUpdateXState &= ~vcMask;
        
        return;
    }
   
    TocinoVCBitSet orig( m_remoteXState );

    // We need to pause this VC
    m_remoteXState &= ~vcMask;

    NS_ASSERT( m_remoteXState != orig );

    m_doUpdateXState |= vcMask;

    Transmit();
}

void TocinoTx::RemoteResume( const uint8_t vc )
{
    // FIXME: cast required to actually print?!
    NS_LOG_FUNCTION( static_cast<unsigned>(vc) );

    // Asserted bits in m_remoteXState indicate XON.
    // Resuming can ONLY assert bits in m_remoteXState
    
    const TocinoVCBitSet vcMask = ( 1 << vc );
   
    const TocinoVCBitSet unsentPauses = ( ~m_remoteXState & m_doUpdateXState );

    if( ( unsentPauses & vcMask ).any() )
    {
        // We had an unsent pause on this VC
        
        // Simply assert remoteXState bit to indicate XON
        m_remoteXState |= vcMask;

        // No longer need to update this VC
        m_doUpdateXState &= ~vcMask;

        return;
    }
    
    TocinoVCBitSet orig( m_remoteXState );
    
    // We need to resume this VC
    m_remoteXState |= vcMask;
    
    NS_ASSERT( m_remoteXState != orig );

    m_doUpdateXState |= vcMask;

    Transmit();
}

Ptr<NetDevice> TocinoTx::GetNetDevice()
{
    return m_tnd;
}

void
TocinoTx::TransmitEnd()
{
  m_state = IDLE;
  Transmit();
}

void
TocinoTx::SendToChannel( Ptr<Packet> f )
{
    // this acts as a mutex on Transmit
    m_state = BUSY;

    if( m_portNumber == m_tnd->GetHostPort() ) 
    {
        // ejection port
        NS_LOG_LOGIC( "ejecting " << GetTocinoFlitIdString( f ) );
        
        // ejection port modeled as having infinite bandwidth and buffering
        // need to keep m_state == BUSY to this point to prevent reentrancy
        m_tnd->EjectFlit( f ); // eject the packet

        // ScheduleNow here, rather than direct call to TransmitEnd
        // avoids mind-bending reentrancy due to:
        //    Transmit -> SendToChannel -> TransmitEnd -> Transmit
        // Otherwise we can end up with multiple Transmits in flight
        // at once, which is very confusing.
        
        Simulator::ScheduleNow( &TocinoTx::TransmitEnd, this );
    }
    else
    {
        // send packet to channel
        NS_ASSERT( m_channel != NULL );
        m_channel->TransmitStart( f );
        Time transmit_time = m_channel->GetTransmissionTime( f );

        NS_LOG_LOGIC( "transmitting " << GetTocinoFlitIdString( f ) 
            << " for " << transmit_time );

        Simulator::Schedule(transmit_time, &TocinoTx::TransmitEnd, this);
    }
}


void
TocinoTx::DoTransmitFlowControl()
{
    NS_LOG_FUNCTION_NOARGS();

    if( m_portNumber != m_tnd->GetHostPort() )
    {
        Ptr<Packet> f = GetTocinoFlowControlFlit( m_remoteXState );
        
        NS_LOG_LOGIC( "sending " << m_remoteXState.to_string() );
        
        SendToChannel( f );
    }
        
    m_doUpdateXState.reset();
}

void
TocinoTx::DoTransmit()
{
    NS_LOG_FUNCTION_NOARGS();

    NS_ASSERT( m_arbiter != NULL );

    uint32_t winner = m_arbiter->Arbitrate();

    if( winner == TocinoArbiter::DO_NOTHING )
    {
        NS_LOG_LOGIC( "Nothing to do" );
        return;
    }
    
    uint32_t rx_port = m_tnd->QueueToPort( winner );

    // N.B.
    //
    // If isNoLongerBlocked == TRUE below, the cause is
    // unambiguous: it must be due to our Dequeue().
    // So, while it would be correct to call:
    //      GetReceiver(rx_port)->IsAnyQueueBlocked()
    // here, instead of:
    //      m_queues[winner]->IsAlmostFull()
    // We use the latter. There is really no need to
    // scan all the receiver's queues.  We know which
    // one matters
    //  -MAS
    bool wasAlmostFull = m_queues[winner]->IsAlmostFull();

    // Dequeue must occur whether the receiver was blocked or not 
    Ptr<Packet> f = m_queues[winner]->Dequeue();
    NS_ASSERT_MSG( f != NULL, "queue underrun" << winner );
            
    bool isNoLongerBlocked = !m_tnd->GetReceiver(rx_port)->IsAnyQueueBlocked();

    SendToChannel( f );

    // If we just unblocked rx_port, resume the corresponding transmitter.
    if( wasAlmostFull && isNoLongerBlocked )
    {
        if( rx_port != m_tnd->GetHostPort() )
        {
            uint8_t vc = m_tnd->QueueToVC( winner );
            m_tnd->GetTransmitter(rx_port)->RemoteResume( vc );
        }
        else
        {
            // Special handling for injection port
            m_tnd->TrySendFlits();
        }
    }
}

void
TocinoTx::Transmit()
{
    if( m_state == BUSY ) 
    {
        return;
    }
    
    NS_LOG_FUNCTION_NOARGS();
    
    if( m_doUpdateXState.any() )
    {
        DoTransmitFlowControl();
    }
    else 
    {
        DoTransmit();
    }
}

bool
TocinoTx::CanTransmitFrom( uint32_t qnum ) const
{
    // We can transmit from a queue iff
    //  -It is not empty
    //  -The corresponding VC is enabled
    
    NS_ASSERT( qnum < m_queues.size() );
   
    if( !m_queues[qnum]->IsEmpty() ) 
    {
        uint8_t vc = m_tnd->QueueToVC( qnum );

        const TocinoVCBitSet vcMask = ( 1 << vc );

        if( ( m_xState & vcMask ).any() )
        {
            return true;
        }
    }

    return false;
}
    
bool
TocinoTx::IsNextFlitHead( uint32_t qnum ) const
{
    NS_ASSERT( qnum < m_queues.size() );
    NS_ASSERT( !m_queues[qnum]->IsEmpty() );

    Ptr<const Packet> f = m_queues[qnum]->Peek();
    TocinoFlitHeader h;
    f->PeekHeader( h );

    return h.IsHead();
}
    
bool
TocinoTx::IsNextFlitTail( uint32_t qnum ) const
{
    NS_ASSERT( qnum < m_queues.size() );
    NS_ASSERT( !m_queues[qnum]->IsEmpty() );

    Ptr<const Packet> f = m_queues[qnum]->Peek();
    TocinoFlitHeader h;
    f->PeekHeader( h );

    return h.IsTail();
}

void
TocinoTx::SetQueue( uint32_t qnum, Ptr<CallbackQueue> q )
{
    NS_ASSERT( qnum < m_queues.size() );
    m_queues[qnum] = q;
}

void
TocinoTx::DumpState()
{
    NS_LOG_LOGIC("transmitter=" << m_portNumber);
    NS_LOG_LOGIC("  xState=" << m_xState);
    for (uint32_t vc = 0; vc < 1; vc++)  // change the loop limit to enable more VCs
    {
        uint32_t owner =  m_arbiter->GetVCOwner(vc);
        const char* xState_str = (m_xState[vc])? "XON":"XOFF";

        if (owner == TocinoSimpleArbiter::ANY_PORT)
        {
            NS_LOG_LOGIC("   vc=" << vc << " owner=NONE " << xState_str);

            // can only schedule head flit - which sources have head flit at front of queue
            for (uint32_t src = 0; src < m_tnd->GetNPorts(); src++)
            {
                uint32_t q = (src * m_tnd->GetNVCs()) + vc;
                if (m_queues[q]->Size() > 0)
                {
                    NS_LOG_LOGIC("   next=" << GetTocinoFlitIdString(m_queues[q]->At(0)) << 
                        " src=" << src);
                }
            }
        }
        else
        {
            NS_LOG_LOGIC("   vc=" << vc << " owner=" << owner << " " << xState_str);

            uint32_t q = (owner * m_tnd->GetNVCs()) + vc;
            if (m_queues[q]->Size())
            {
                NS_LOG_LOGIC("   next=" << GetTocinoFlitIdString(m_queues[q]->At(0)));
            }
            else
            {
                NS_LOG_LOGIC("    empty queue");
            }
        }
    }
}

} // namespace ns3

#pragma pop_macro("NS_LOG_APPEND_CONTEXT")

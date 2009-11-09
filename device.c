/*
 * (c) BayCom GmbH, http://www.baycom.de, info@baycom.de
 *
 * See the COPYING file for copyright information and
 * how to reach the author.
 *
 */

#include <time.h>
#include <iostream>

#include <vdr/channels.h>
#include <vdr/ringbuffer.h>
#include <vdr/eit.h>
#include <vdr/timers.h>
#include <vdr/skins.h>

#include "filter.h"
#include "device.h"
#include "mcli.h"

#define st_Pos  0x07FF
#define st_Neg  0x0800

//#define DEBUG_PIDS 
//#define DEBUG_TUNE

#define TEMP_DISABLE_TIMEOUT_DEFAULT (10)
#define TEMP_DISABLE_TIMEOUT_SCAN (30)
#define LASTSEEN_TIMEOUT (7)
//#define ENABLE_DEVICE_PRIORITY

using namespace std;

static int handle_ts (unsigned char *buffer, size_t len, void *p)
{
	return p ? ((cMcliDevice *) p)->HandleTsData (buffer, len) : len;
}

//-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

static int handle_ten (tra_t * ten, void *p)
{
	cMcliDevice *m = (cMcliDevice *) p;
	if (ten) {
//              fprintf (stderr, "Status: %02X, Strength: %04X, SNR: %04X, BER: %04X\n", ten->s.st, ten->s.strength, ten->s.snr, ten->s.ber);
		m->SetTenData (ten);
		if (ten->s.st & FE_HAS_LOCK) {
			m->m_locked.Broadcast ();
		}
	} else {
		tra_t ten;
		memset (&ten, 0, sizeof (tra_t));
		m->SetTenData (&ten);
//              fprintf (stderr, "Signal lost\n");
	}
	return 0;
}

cMcliDevice::cMcliDevice (void)
{
	m_enable = false;
	m_tuned = false;
	StartSectionHandler ();
	m_PB = new cMyPacketBuffer (10000 * TS_SIZE, 10000);
	m_PB->SetTimeouts (0, 1000 * 20);
	m_filters = new cMcliFilters ();
//	printf ("cMcliDevice: got device number %d\n", CardIndex () + 1);
	m_pidsnum = 0;
	m_mcpidsnum = 0;
	m_filternum = 0;
	m_chan = NULL;
	m_mcli = NULL;
	m_fetype = -1;
	m_last = 0;
	m_showtuning = 0;
	m_ca_enable = false;
	m_ca_override = false;
	memset (m_pids, 0, sizeof (m_pids));
	memset (&m_ten, 0, sizeof (tra_t));
	m_pids[0].pid=-1;
	m_disabletimeout = TEMP_DISABLE_TIMEOUT_DEFAULT;
	m_tunerref = NULL;
	InitMcli ();
}

cMcliDevice::~cMcliDevice ()
{
	LOCK_THREAD;
	StopSectionHandler ();
	printf ("Device %d gets destructed\n", CardIndex () + 1);
	Cancel (0);
	m_locked.Broadcast ();
	ExitMcli ();
	DELETENULL (m_filters);
	DELETENULL (m_PB);
}

void cMcliDevice::SetTenData (tra_t * ten)
{
	if(!ten->lastseen) {
		ten->lastseen=m_ten.lastseen;
	}
	m_ten = *ten;
}

//-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void cMcliDevice::SetEnable (bool val)
{
	LOCK_THREAD;
	m_enable = val;
	if (!m_enable) {
		recv_stop (m_r);
		m_tuned = false;
		if(GetCaEnable()) {
			SetCaEnable(false);
			m_mcli->CAMFree();
		}
		if(m_tunerref) {
			m_mcli->TunerFree(m_tunerref);
			m_tunerref = NULL;
			m_fetype = -1;
		}
	} else {
		if (m_chan) {
			if(m_tunerref == NULL) {
#if VDRVERSNUM < 10702	
				bool s2=m_chan->Modulation() == QPSK_S2 || m_chan->Modulation() == PSK8;
#else	
				bool s2=m_chan->System() == SYS_DVBS2;
#endif
				bool ret = false;
				int pos;
				int type;
				
				TranslateTypePos(type, pos, m_chan->Source());
				if(s2) {
					type=FE_DVBS2;
				}
				ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
				if(!ret && type == FE_QPSK) {
					type = FE_DVBS2;
					ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
				}
				if(!ret) {
					return;
				}
				m_fetype = type;
			}
			if(m_chan->Ca() && !GetCaEnable() && m_mcli->CAMAvailable() && m_mcli->CAMAlloc()) {
				SetCaEnable();
			}

			recv_tune (m_r, (fe_type_t)m_fetype, m_pos, &m_sec, &m_fep, m_pids);
			m_tuned = true;
		}
	}
}

bool cMcliDevice::SetTempDisable (bool now)
{
	if(!now) {
		Lock();
	}
#ifndef REELVDR // they might find it out in some other place
	// Check for tuning timeout
	if(m_showtuning && Receiving(false) && ((time(NULL)-m_ten.lastseen)>=LASTSEEN_TIMEOUT)) {
		if(m_chan) {
			Skins.QueueMessage(mtInfo, cString::sprintf(tr("Waiting for a free tuner (%s)"),m_chan->Name()));
		}
		m_showtuning = false;
	}
#endif
//	printf("Device %d Receiving %d Priority %d\n",CardIndex () + 1, Receiving (true), Priority());
	if(!Receiving (true) && (((time(NULL)-m_last) >= m_disabletimeout)) || now) {
		recv_stop (m_r);
		m_tuned = false;
		if(GetCaEnable()) {
			SetCaEnable(false);
			m_mcli->CAMFree();
		}
		if(m_tunerref) {
#ifdef DEBUG_TUNE
			printf("Releasing tuner on %d (%s)\n",CardIndex()+1, m_chan->Name());
#endif			
			m_mcli->TunerFree(m_tunerref, false);
			m_tunerref = NULL;
			m_fetype = -1;
			m_chan = NULL;
		}
		if(!now) {
			Unlock();
		}
		return true;
	}
	if(!now) {
		Unlock();
	}
	return false;
}

void cMcliDevice::SetFEType (fe_type_t val)
{
	m_fetype = (int)val;
}

int cMcliDevice::HandleTsData (unsigned char *buffer, size_t len)
{
	m_filters->PutTS (buffer, len);
#ifdef GET_TS_PACKETS
	unsigned char *ptr = m_PB->PutStart (len);
	if (ptr) {
		memcpy (ptr, buffer, len);
		m_PB->PutEnd (len, 0, 0);
	}
#else
	unsigned int i;
	for (i = 0; i < len; i += TS_SIZE) {
		unsigned char *ptr = m_PB->PutStart (TS_SIZE);
		if (ptr) {
			memcpy (ptr, buffer + i, TS_SIZE);
			m_PB->PutEnd (TS_SIZE, 0, 0);
		}
	}
#endif
	return len;
}


void cMcliDevice::InitMcli (void)
{
	m_r = recv_add ();

	register_ten_handler (m_r, handle_ten, this);
	register_ts_handler (m_r, handle_ts, this);
}

void cMcliDevice::ExitMcli (void)
{
	register_ten_handler (m_r, NULL, NULL);
	register_ts_handler (m_r, NULL, NULL);
	recv_del (m_r);
	m_r = NULL;
}

bool cMcliDevice::ProvidesSource (int Source) const
{
	int pos;
	int type;
	bool ret=false;

	if (!m_enable) {
		return false;
	}
	
	TranslateTypePos(type, pos, Source);

	if(m_tunerref) {
		ret= (type == m_fetype) || (type == FE_QPSK && m_fetype == FE_DVBS2);
		if(ret) {
			ret = m_mcli->TunerSatelitePositionLookup(m_tunerref, pos);
		}
	}
	
	if(!ret) {
		ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
		if(!ret && type == FE_QPSK) {
			type = FE_DVBS2;
			ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
		}
	} 
#ifdef DEBUG_TUNE
	printf ("ProvidesSource %d Type %d Pos %d -> %d\n", CardIndex () + 1, type, pos, ret);
#endif
	return ret;
}

bool cMcliDevice::ProvidesTransponder (const cChannel * Channel) const
{
//      printf ("ProvidesTransponder %s\n", Channel->Name ());
	if (!m_enable) {
		return false;
	}
#if VDRVERSNUM < 10702	
	bool s2=Channel->Modulation() == QPSK_S2 || Channel->Modulation() == PSK8;
#else	
	bool s2=Channel->System() == SYS_DVBS2;
#endif	
	bool ret=ProvidesSource (Channel->Source ());
	if(ret) {
		int pos;
		int type;
		TranslateTypePos(type, pos, Channel->Source());
		if(s2) {
			type=FE_DVBS2;
		}
		if(m_tunerref) {
			ret = (m_fetype == type) || (type == FE_QPSK && m_fetype == FE_DVBS2);
		} else {
			ret = false;
		}
		if(!ret) {
			ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
			if(!ret && type == FE_QPSK) {
				type = FE_DVBS2;
				ret = m_mcli->TunerAvailable((fe_type_t)type, pos);
			}
		}
	}
#ifdef DEBUG_TUNE
	printf ("ProvidesTransponder %d S2:  %d %s@%p -> %d\n", CardIndex () + 1, s2, Channel->Name (), this, ret);
#endif
	return ret;
}

bool cMcliDevice::IsTunedToTransponderConst (const cChannel * Channel) const
{
//      printf ("IsTunedToTransponder %s == %s \n", Channel->Name (), m_chan ? m_chan->Name () : "");
	if (!m_enable || !m_tuned || !m_chan) {
		return false;
	}

	if (m_ten.s.st & FE_HAS_LOCK && m_chan->Source() == Channel->Source() &&
	        m_chan->Transponder() == Channel->Transponder() && m_chan->Frequency() == Channel->Frequency() &&
	                m_chan->Modulation() == Channel->Modulation() &&
	                        m_chan->Srate() == Channel->Srate()) {
//              printf ("Yes!!!");
		return true;
	}
//      printf ("Nope!!!");
	return false;
}

bool cMcliDevice::IsTunedToTransponder (const cChannel * Channel)
{
	return IsTunedToTransponderConst(Channel);
}

bool cMcliDevice::CheckCAM(const cChannel * Channel, bool steal) const
{
	if(GetCaOverride()) {
		return true;
	}

	if(Channel->Ca() && !GetCaEnable() && !m_mcli->CAMAvailable() && !m_mcli->StealCAM(steal)) {
		return false;
	}
	return true;
}
 
bool cMcliDevice::ProvidesChannel (const cChannel * Channel, int Priority, bool * NeedsDetachReceivers) const
{
	bool result = false;
	bool hasPriority = Priority < 0 || Priority > this->Priority ();
	bool needsDetachReceivers = false;
	if (!m_enable) {
		return false;
	}
	if(!CheckCAM(Channel, false)) {
#ifdef DEBUG_TUNE
		printf ("ProvidesChannel %d Channel=%s, Prio=%d this->Prio=%d -> %d\n", CardIndex () + 1, Channel->Name (), Priority, this->Priority (), false);
#endif
		return false;
	}
	if(ProvidesTransponder(Channel)) {
		result = hasPriority;
		if (Priority >= 0 && Receiving (true))
		{
			if (!IsTunedToTransponderConst(Channel)) {
				needsDetachReceivers = true;
			} else {
				result = true;
			}
		}
	}
#ifdef DEBUG_TUNE
	printf ("ProvidesChannel %d Channel=%s, Prio=%d this->Prio=%d NeedsDetachReceivers: %d -> %d\n", CardIndex () + 1, Channel->Name (), Priority, this->Priority (), needsDetachReceivers, result);
#endif
	if (NeedsDetachReceivers) {
		*NeedsDetachReceivers = needsDetachReceivers;
	}
	return result;
}

void cMcliDevice::TranslateTypePos(int &type, int &pos, const int Source) const
{
	pos = Source;
	pos = ((pos & st_Neg) ? 1 : -1) * (pos & st_Pos);
//      printf ("Position: %d\n", spos);
	if (pos) {
		pos += 1800;
	} else {
		pos = NO_SAT_POS;
	}
	
	type = Source & cSource::st_Mask;
	switch(type) {
		case cSource::stCable: 
			type = FE_QAM;
			break;
		case cSource::stSat:
			type = FE_QPSK;
			break;
		case cSource::stTerr:
			type = FE_OFDM;
			break;
		default:
			type = -1;
	}
}

bool cMcliDevice::SetChannelDevice (const cChannel * Channel, bool LiveView)
{
	bool is_scan = false;
	int pos;
	int type;
	bool s2;
	
#ifdef DEBUG_TUNE
	printf ("SetChannelDevice %d Channel(%p): %s, Provider: %s, Source: %d, LiveView: %s, IsScan: %d\n", CardIndex () + 1, Channel, Channel->Name (), Channel->Provider (), Channel->Source (), LiveView ? "true" : "false", is_scan);
#endif
	if (!m_enable) {
		return false;
	}
	LOCK_THREAD;
	TranslateTypePos(type, pos, Channel->Source());

	is_scan = !strlen(Channel->Name()) && !strlen(Channel->Provider());
	if(is_scan) {
		m_disabletimeout = TEMP_DISABLE_TIMEOUT_SCAN;
	} else {
		m_disabletimeout = TEMP_DISABLE_TIMEOUT_DEFAULT;
	}

#if VDRVERSNUM < 10702	
	s2=Channel->Modulation() == QPSK_S2 || Channel->Modulation() == PSK8;
#else	
	s2=Channel->System() == SYS_DVBS2;
#endif	
	if(!CheckCAM(Channel, true)) {
#ifdef DEBUG_TUNE
		printf("No CAM on %d available even after tried to steal one\n", CardIndex () + 1);
#endif
		return false;
	}
	if(!GetCaOverride() && Channel->Ca() && !GetCaEnable()) {
		if(!m_mcli->CAMAlloc()) {
#ifdef DEBUG_TUNE
			printf("failed to get CAM on %d\n",CardIndex () + 1);
#endif
			return false;
		}
		SetCaEnable();
	}

	if(m_tunerref && (m_fetype != type || !m_mcli->TunerSatelitePositionLookup(m_tunerref, pos))) {
		m_mcli->TunerFree(m_tunerref);
		m_tunerref = NULL;
	}
	
	if(s2 && (m_fetype != FE_DVBS2)) {
		if(m_tunerref) {
			m_mcli->TunerFree(m_tunerref);
			m_tunerref = NULL;
		}
		type=FE_DVBS2;
	}

	if(m_tunerref == NULL) {
		m_tunerref = m_mcli->TunerAlloc((fe_type_t)type, pos);
		if(m_tunerref == NULL && type == FE_QPSK) {
			type = FE_DVBS2;
			m_tunerref = m_mcli->TunerAlloc((fe_type_t)type, pos);
		}
		if(m_tunerref == NULL) {
			return false;
		}
		m_fetype = type;
	}


	if (IsTunedToTransponder (Channel) && !is_scan) {
#ifdef DEBUG_TUNE
                printf("Already tuned to transponder on %d\n",CardIndex () + 1);
#endif
		m_chan = Channel;
		m_pos = pos;
		return true;
	} else {
		memset (&m_ten, 0, sizeof (tra_t));
	}
	memset (&m_sec, 0, sizeof (recv_sec_t));
	memset (&m_fep, 0, sizeof (struct dvb_frontend_parameters));

	m_chan = Channel;
	m_pos = pos;
//	printf("Really tuning on %d\n",CardIndex () + 1);
	switch (m_fetype) {
	case FE_DVBS2:
	case FE_QPSK:{		// DVB-S

			unsigned int frequency = Channel->Frequency ();

			fe_sec_voltage_t volt = (Channel->Polarization () == 'v' || Channel->Polarization () == 'V' || Channel->Polarization () == 'r' || Channel->Polarization () == 'R') ? SEC_VOLTAGE_13 : SEC_VOLTAGE_18;
			m_sec.voltage = volt;
			frequency =::abs (frequency);	// Allow for C-band, where the frequency is less than the LOF
			m_fep.frequency = frequency * 1000UL;
			m_fep.inversion = fe_spectral_inversion_t (Channel->Inversion ());
			m_fep.u.qpsk.symbol_rate = Channel->Srate () * 1000UL;
#if VDRVERSNUM < 10702				
			m_fep.u.qpsk.fec_inner = fe_code_rate_t (Channel->CoderateH () | (Channel->Modulation () << 16));
#else
			if(s2) {
				int modulation = 0;
				switch(Channel->Modulation ()) {
					case QPSK:
						modulation = QPSK_S2;
						break;
					case PSK_8:
						modulation = PSK8;
						break;
				}
				 m_fep.u.qpsk.fec_inner = fe_code_rate_t (Channel->CoderateH () | (modulation << 16));
			}
#endif			
		}
		break;
	case FE_QAM:{		// DVB-C

			// Frequency and symbol rate:
			m_fep.frequency = FrequencyToHz (Channel->Frequency ());
			m_fep.inversion = fe_spectral_inversion_t (Channel->Inversion ());
			m_fep.u.qam.symbol_rate = Channel->Srate () * 1000UL;
			m_fep.u.qam.fec_inner = fe_code_rate_t (Channel->CoderateH ());
			m_fep.u.qam.modulation = fe_modulation_t (Channel->Modulation ());
		}
		break;
	case FE_OFDM:{		// DVB-T

			// Frequency and OFDM paramaters:
			m_fep.frequency = FrequencyToHz (Channel->Frequency ());
			m_fep.inversion = fe_spectral_inversion_t (Channel->Inversion ());
			m_fep.u.ofdm.bandwidth = fe_bandwidth_t (Channel->Bandwidth ());
			m_fep.u.ofdm.code_rate_HP = fe_code_rate_t (Channel->CoderateH ());
			m_fep.u.ofdm.code_rate_LP = fe_code_rate_t (Channel->CoderateL ());
			m_fep.u.ofdm.constellation = fe_modulation_t (Channel->Modulation ());
			m_fep.u.ofdm.transmission_mode = fe_transmit_mode_t (Channel->Transmission ());
			m_fep.u.ofdm.guard_interval = fe_guard_interval_t (Channel->Guard ());
			m_fep.u.ofdm.hierarchy_information = fe_hierarchy_t (Channel->Hierarchy ());
		}
		break;
	default:
		esyslog ("ERROR: attempt to set channel with unknown DVB frontend type");
		return false;
	}

	recv_tune (m_r, (fe_type_t)m_fetype, m_pos, &m_sec, &m_fep, m_pids);
	m_tuned = true;
	if((m_pids[0].pid==-1)) {
		dvb_pid_t pi;
		memset(&pi, 0, sizeof(dvb_pid_t));
		recv_pid_add (m_r, &pi);
//		printf("add dummy pid 0 @ %p\n", this);
	}
#ifdef DEBUG_PIDS
	printf ("%p SetChannelDevice: Pidsnum: %d m_pidsnum: %d\n", m_r, m_mcpidsnum, m_pidsnum);
	for (int i = 0; i < m_mcpidsnum; i++) {
		printf ("Pid: %d\n", m_pids[i].pid);
	}
#endif
	m_ten.lastseen=m_last=time(NULL);
	m_showtuning = true;
	return true;
}

bool cMcliDevice::HasLock (int TimeoutMs)
{
//      printf ("HasLock TimeoutMs:%d\n", TimeoutMs);

	if ((m_ten.s.st & FE_HAS_LOCK) || !TimeoutMs) {
		return m_ten.s.st & FE_HAS_LOCK;
	}
	cMutexLock MutexLock (&mutex);
	if (TimeoutMs && !(m_ten.s.st & FE_HAS_LOCK)) {
		m_locked.TimedWait (mutex, TimeoutMs);
	}
	if (m_ten.s.st & FE_HAS_LOCK) {
		return true;
	}
	return false;
}

bool cMcliDevice::SetPid (cPidHandle * Handle, int Type, bool On)
{
#ifdef DEBUG_TUNE
	printf ("SetPid %d Pid=%d (%s), Type=%d, On=%d, used=%d %d %d %d %d\n",  CardIndex () + 1, Handle->pid, m_chan->Name(), Type, On, Handle->used, ptAudio, ptVideo, ptDolby, ptOther);
#endif
	dvb_pid_t pi;
	memset (&pi, 0, sizeof (dvb_pid_t));
	if (!m_enable) {
		return false;
	}
	LOCK_THREAD;
	if (Handle->pid && (On || !Handle->used)) {
		m_pidsnum += On ? 1 : -1;
		if (m_pidsnum < 0) {
			m_pidsnum = 0;
		}

		if (On) {
			pi.pid = Handle->pid;
			if (GetCaEnable() && m_chan && m_chan->Ca (0)) {
				pi.id= m_chan->Sid();
				if(m_chan->Ca(0)<=0xff) {
					pi.priority=m_chan->Ca(0)&0x03;
				}
			} 
#ifdef ENABLE_DEVICE_PRIORITY
			int Prio = Priority();
			if(Prio>50) // Recording prio high
				pi.priority |= 3<<2;
			else if(Prio > 10) // Recording prio normal
				pi.priority |= 2<<2;
			else if(Prio >= 0) // Recording prio low
				pi.priority |= 1<<2;
			else if(Prio == -1) // Live
				pi.priority |= 1<<2;
#endif
//			printf ("Add Pid: %d Sid:%d Type:%d Prio: %d %d\n", pi.pid, pi.id, Type, pi.priority, m_chan ? m_chan->Ca(0) : -1);
			recv_pid_add (m_r, &pi);
		} else {
//                     	printf ("Del Pid: %d\n", Handle->pid);
			recv_pid_del (m_r, Handle->pid);
		}
	}
	m_mcpidsnum = recv_pids_get (m_r, m_pids);
	if(!m_mcpidsnum) {
		printf("##########################################    Disable CA\n");
		if(GetCaEnable()) {
			SetCaEnable(false);
			m_mcli->CAMFree();
		}
	}
#ifdef DEBUG_PIDS
	printf ("%p SetPid: Pidsnum: %d m_pidsnum: %d m_filternum: %d\n", m_r, m_mcpidsnum, m_pidsnum, m_filternum);
	for (int i = 0; i < m_mcpidsnum; i++) {
		printf ("Pid: %d\n", m_pids[i].pid);
	}
#endif
	m_last=time(NULL);
	return true;
}

bool cMcliDevice::OpenDvr (void)
{
//	printf ("OpenDvr\n");
	m_dvr_open = true;
	return true;
}

void cMcliDevice::CloseDvr (void)
{
//	printf ("CloseDvr\n");
	m_dvr_open = false;
}

#ifdef GET_TS_PACKETS
int cMcliDevice::GetTSPackets (uchar * Data, int count)
{
	if (!m_enable || !m_dvr_open) {
		return 0;
	}
	m_PB->GetEnd ();

	int size;
	uchar *buf = m_PB->GetStartMultiple (count, &size, 0, 0);
	if (buf) {
		memcpy (Data, buf, size);
		m_PB->GetEnd ();
		return size;
	} else {
		return 0;
	}
}				// cMcliDevice::GetTSPackets
#endif

bool cMcliDevice::GetTSPacket (uchar * &Data)
{
	if (m_enable && m_dvr_open) {
		m_PB->GetEnd ();

		int size;
		Data = m_PB->GetStart (&size, 0, 0);
	}
	return true;
}

int cMcliDevice::OpenFilter (u_short Pid, u_char Tid, u_char Mask)
{
	if (!m_enable) {
		return -1;
	}
	LOCK_THREAD;
	m_filternum++;
//	printf ("OpenFilter (%d/%d/%d) pid:%d tid:%d mask:%04x %s\n", m_filternum, m_pidsnum, m_mcpidsnum, Pid, Tid, Mask, ((m_filternum+m_pidsnum) < m_mcpidsnum) ? "PROBLEM!!!":"");
	dvb_pid_t pi;
	memset (&pi, 0, sizeof (dvb_pid_t));
	pi.pid = Pid;
//      printf ("Add Pid: %d\n", pi.pid);
	recv_pid_add (m_r, &pi);
	m_mcpidsnum = recv_pids_get (m_r, m_pids);
#ifdef DEBUG_PIDS
	printf ("%p OpenFilter: Pidsnum: %d m_pidsnum: %d\n", m_r, m_mcpidsnum, m_pidsnum);
	for (int i = 0; i < m_mcpidsnum; i++) {
		printf ("Pid: %d\n", m_pids[i].pid);
	}
#endif
	return m_filters->OpenFilter (Pid, Tid, Mask);
}

void cMcliDevice::CloseFilter (int Handle)
{
	if (!m_enable) {
		return;
	}

	LOCK_THREAD;
	int pid = m_filters->CloseFilter (Handle);
	
	if ( pid != -1) {
//		printf("CloseFilter FULL\n");
		recv_pid_del (m_r, pid);
		m_mcpidsnum = recv_pids_get (m_r, m_pids);
	}
	m_filternum--;
//	printf ("CloseFilter(%d/%d/%d) pid:%d %s\n", m_filternum, m_pidsnum, m_mcpidsnum, pid, pid==-1?"PID STILL USED":"");
}

#ifdef DEVICE_ATTRIBUTES
/* Attribute classes for dvbdevice
  main  main attributes
      .name (String) "DVB", "IPTV", ...

  fe : frontend attributes (-> get from tuner)
      .type (int) FE_QPSK, ...
      .name (string) Tuner name
      .status,.snr,... (int)
*/
int cMcliDevice::GetAttribute (const char *attr_name, uint64_t * val)
{
	int ret = 0;
	uint64_t rval = 0;

	if (!strcmp (attr_name, "fe.status")) {
		rval = m_ten.s.st;
	} else if (!strcmp (attr_name, "fe.signal")) {
		rval = m_ten.s.strength;
	} else if (!strcmp (attr_name, "fe.snr")) {
		rval = m_ten.s.snr;
	} else if (!strcmp (attr_name, "fe.ber")) {
		rval = m_ten.s.ber;
	} else if (!strcmp (attr_name, "fe.unc")) {
		rval = m_ten.s.ucblocks;
	} else if (!strcmp (attr_name, "fe.type")) {
		rval = m_fetype;
	} else if (!strcmp (attr_name, "is.mcli")) {
		rval = 1;
	} else if (!strcmp (attr_name, "fe.lastseen")) {
		rval = m_ten.lastseen;
	} else
		ret = -1;

	if (val)
		*val = rval;
	return ret;
}

int cMcliDevice::GetAttribute (const char *attr_name, char *val, int maxret)
{
	int ret = 0;
	if (!strcmp (attr_name, "fe.uuid")) {
		strncpy (val, "NetCeiver", maxret);
		val[maxret - 1] = 0;
	} else if (!strcmp (attr_name, "fe.name")) {
		strncpy (val, "NetCeiver", maxret);
		val[maxret - 1] = 0;
	} else if (!strncmp (attr_name, "main.", 5)) {
		if (!strncmp (attr_name + 5, "name", 4)) {
			if (val && maxret > 0) {
				strncpy (val, "NetCeiver", maxret);
				val[maxret - 1] = 0;
			}
			return 0;
		}
	} else {
		ret = -1;
	}
	return ret;
}
#endif

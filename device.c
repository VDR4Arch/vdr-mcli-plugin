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

satellite_list_t *cMcliDevice::find_sat_list (netceiver_info_t * nc_info, const char *SatelliteListName) const
{
	int i;

//	printf ("SatelliteListName %s\n", SatelliteListName);
	if(SatelliteListName==NULL) {
		return NULL;
	}

	for (i = 0; i < nc_info->sat_list_num; i++) {
		if (!strcmp (SatelliteListName, nc_info->sat_list[i].Name)) {
//			printf ("found uuid in sat list %d\n", i);
			return nc_info->sat_list + i;
		}
	}
	return NULL;
}

bool cMcliDevice::SatelitePositionLookup(satellite_list_t *satlist, int pos) const
{
	int i;
//	printf("SatelitePositionLookup: %p %d\n",satlist, pos);
	if(satlist == NULL) {
		return false;
	}
	for(i=0; i<satlist->sat_num;i ++) {
		satellite_info_t *s=satlist->sat+i;
		switch(s->type){
			case SAT_SRC_LNB:
				if(pos == s->SatPos) {
//					printf("satlist found\n");
					return true;
				}
				break;
			case SAT_SRC_ROTOR:
				if(pos>=s->SatPosMin && pos <=s->SatPosMax) {
//					printf("satlist found\n");
					return true;
				}
				break;
		}
	}
//	printf("satlist not found\n");
	
	return false;
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
			m_mcli->FreeCAM();
		}
	} else {
		if (m_chan) {
			if(m_chan->Ca() && !GetCaEnable() && m_mcli->AvailableCAMs() && m_mcli->AllocCAM()) {
				SetCaEnable();
			}

			recv_tune (m_r, m_fetype, m_pos, &m_sec, &m_fep, m_pids);
			m_tuned = true;
		}
	}
}

bool cMcliDevice::SetTempDisable (void)
{
	LOCK_THREAD;
#ifndef REELVDR // they might find it out in some other place
	// Check for tuning timeout
	if(m_showtuning && Receiving(true) && ((time(NULL)-m_ten.lastseen)>=LASTSEEN_TIMEOUT)) {
		if(m_chan) {
			Skins.QueueMessage(mtInfo, cString::sprintf(tr("Waiting for a free tuner (%s)"),m_chan->Name()));
		}
		m_showtuning = false;
	}
#endif
	if(!Receiving (true) && ((time(NULL)-m_last) >= m_disabletimeout)) {
		recv_stop (m_r);
		m_tuned = false;
		if(GetCaEnable()) {
			SetCaEnable(false);
			m_mcli->FreeCAM();
		}
		return true;
	}
	return false;
}

void cMcliDevice::SetFEType (fe_type_t val)
{
	m_fetype = val;
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

cMcliDevice::cMcliDevice (void)
{
	m_enable = false;
	m_tuned = false;
	StartSectionHandler ();
	m_PB = new cMyPacketBuffer (10000 * TS_SIZE, 10000);
	m_PB->SetTimeouts (0, 1000 * 20);
	m_filters = new cMcliFilters ();
	printf ("cMcliDevice: got device number %d\n", CardIndex () + 1);
	m_pidsnum = 0;
	m_mcpidsnum = 0;
	m_filternum = 0;
	m_chan = NULL;
	m_mcli = NULL;
	memset(m_satlistname, 0, UUID_SIZE+1);
	m_fetype = FE_QPSK;
	m_last = 0;
	m_showtuning = 0;
	m_ca_enable = false;
	m_ca_override = false;
	memset (m_pids, 0, sizeof (m_pids));
	memset (&m_ten, 0, sizeof (tra_t));
	m_pids[0].pid=-1;
	m_disabletimeout = TEMP_DISABLE_TIMEOUT_DEFAULT;
	InitMcli ();
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

bool cMcliDevice::ProvidesSource (int Source) const
{
//      printf ("ProvidesSource, Source=%d\n", Source);
	if (!m_enable) {
		return false;
	}
	int type = Source & cSource::st_Mask;
	bool ret= type == cSource::stNone || (type == cSource::stCable && m_fetype == FE_QAM) || (type == cSource::stSat && m_fetype == FE_QPSK) || (type == cSource::stSat && m_fetype == FE_DVBS2) || (type == cSource::stTerr && m_fetype == FE_OFDM);
	if(ret && (type == cSource::stSat)) {
		int pos = ((Source & st_Neg) ? 1 : -1) * (Source & st_Pos);
		if (pos) {
			nc_lock_list ();
			netceiver_info_list_t *nc_list = nc_get_list ();
			satellite_list_t *satlist=NULL;
			for (int n = 0; n < nc_list->nci_num; n++) {
				netceiver_info_t *nci = nc_list->nci + n;
			      	satlist=find_sat_list(nci, m_satlistname);
			      	if(satlist) {
			      		break;
			      	}
			}
			nc_unlock_list ();
		      	if(satlist == NULL) {
		      		return false;
			}
			return SatelitePositionLookup(satlist, pos+1800);
		}
	}
	return ret;
}

bool cMcliDevice::ProvidesTransponder (const cChannel * Channel) const
{
//      printf ("ProvidesTransponder %s\n", Channel->Name ());
	if (!m_enable) {
		return false;
	}
	bool ret=ProvidesSource (Channel->Source ()) && ((Channel->Modulation() == QPSK_S2 || Channel->Modulation() == PSK8) ? (m_fetype == FE_DVBS2) : true);
//	printf ("ProvidesTransponder Modulation:  %d %s@%p -> %d\n", Channel->Modulation(), Channel->Name (), this, ret);
	return ret;
}

bool cMcliDevice::IsTunedToTransponder (const cChannel * Channel)
{
//      printf ("IsTunedToTransponder %s == %s \n", Channel->Name (), m_chan ? m_chan->Name () : "");
	if (!m_enable) {
		return false;
	}

	if (m_tuned && m_chan && (Channel->Transponder () == m_chan->Transponder ())) {
//              printf ("Yes!!!");
		return true;
	}
//      printf ("Nope!!!");
	return false;
}

bool cMcliDevice::CheckCAM(const cChannel * Channel) const
{
	if(GetCaOverride()) {
		return true;
	}
	
	if(Channel->Ca() && !GetCaEnable() && !m_mcli->AvailableCAMs()) {
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
	if(!CheckCAM(Channel)) {
//		printf("ProvidesChannel: no CA configured here\n");
		return false;
	}

//      printf ("ProvidesChannel, Channel=%s, Prio=%d this->Prio=%d\n", Channel->Name (), Priority, this->Priority ());
	if(ProvidesTransponder(Channel)) {
		result = hasPriority;
		if (Priority >= 0 && Receiving (true))
		{
			if (m_chan && (Channel->Transponder () != m_chan->Transponder ())) {
				needsDetachReceivers = true;
			} else {
				result = true;
			}
		}
	}
//      printf ("NeedsDetachReceivers: %d\n", needsDetachReceivers);
//      printf ("Result: %d\n", result);
	if (NeedsDetachReceivers) {
		*NeedsDetachReceivers = needsDetachReceivers;
	}
	return result;
}

bool cMcliDevice::SetChannelDevice (const cChannel * Channel, bool LiveView)
{
	int is_scan = !strlen(Channel->Name()) && !strlen(Channel->Provider());
	if(is_scan) {
		m_disabletimeout = TEMP_DISABLE_TIMEOUT_SCAN;
	} else {
		m_disabletimeout = TEMP_DISABLE_TIMEOUT_DEFAULT;
	}
	printf ("SetChannelDevice Channel(%p@%p): %s, Provider: %s, Source: %d, LiveView: %s, IsScan: %d\n", Channel, this, Channel->Name (), Channel->Provider (), Channel->Source (), LiveView ? "true" : "false", is_scan);
	
	if (!m_enable) {
		return false;
	}
	if(!CheckCAM(Channel)) {
//		printf("SetChannelDevice: no CA configured here\n");
		return false;
	}
	if(!GetCaOverride() && Channel->Ca() && !GetCaEnable()) {
		if(!m_mcli->AllocCAM()) {
			return false;
		}
		SetCaEnable();
	}

	LOCK_THREAD;
	if (IsTunedToTransponder (Channel) && !is_scan) {
//              printf("Already tuned to transponder\n");
		m_chan = Channel;
		return true;
	}
	memset (&m_sec, 0, sizeof (recv_sec_t));
	memset (&m_fep, 0, sizeof (struct dvb_frontend_parameters));
	if (is_scan || !IsTunedToTransponder (Channel)) {
		memset (&m_ten, 0, sizeof (tra_t));
	}
	m_chan = Channel;

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
			m_fep.u.qpsk.fec_inner = fe_code_rate_t (Channel->CoderateH () | (Channel->Modulation () << 16));
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

	int spos = Channel->Source ();
	m_pos = ((spos & st_Neg) ? 1 : -1) * (spos & st_Pos);
//      printf ("Position: %d\n", pos);
	if (m_pos) {
		m_pos += 1800;
	} else {
		m_pos = NO_SAT_POS;
	}

	recv_tune (m_r, m_fetype, m_pos, &m_sec, &m_fep, m_pids);
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
//	printf ("SetPid, Pid=%d, Type=%d, On=%d, used=%d %d %d %d %d\n", Handle->pid, Type, On, Handle->used, ptAudio, ptVideo, ptDolby, ptOther);
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
	printf ("OpenDvr\n");
	m_dvr_open = true;
	return true;
}

void cMcliDevice::CloseDvr (void)
{
	printf ("CloseDvr\n");
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

void cMcliDevice::SetUUID (const char *uuid)
{
	strncpy (m_uuid, uuid, UUID_SIZE);
	m_uuid[UUID_SIZE]=0;
}

void cMcliDevice::SetSateliteListName(char *s)
{
	strncpy(m_satlistname, s, UUID_SIZE);
	m_satlistname[UUID_SIZE]=0;
}

const char *cMcliDevice::GetUUID (void)
{
	return m_uuid;
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
		strncpy (val, m_uuid, maxret);
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

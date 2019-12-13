/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *      Copyright (C) 2008-2009 Andrej Stepanchuk
 *      Copyright (C) 2009 Howard Chu
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RTMPDump; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>

#ifdef WIN32
#include <winsock.h>
#define close(x)	closesocket(x)
#else
#include <sys/times.h>
#endif

#include "rtmp2.h"
#include "log.h"
#include "bytes.h"

#define RTMP_SIG_SIZE 1536
#define RTMP_LARGE_HEADER_SIZE 12

static const int packetSize[] = { 12, 8, 4, 1 };
#define RTMP_PACKET_SIZE_LARGE    0
#define RTMP_PACKET_SIZE_MEDIUM   1
#define RTMP_PACKET_SIZE_SMALL    2
#define RTMP_PACKET_SIZE_MINIMUM  3

extern bool bCtrlC;

char RTMPProtocolStrings[][7] =
{
	"RTMP",
	"RTMPT",
	"RTMPS",
	"RTMPE",
	"RTMPTE",
	"RTMFP"
};

char RTMPProtocolStringsLower[][7] =
{
        "rtmp",
        "rtmpt",
        "rtmps",
        "rtmpe",
        "rtmpte",
        "rtmpfp"
};

static bool DumpMetaData(AMFObject *obj);
static bool HandShake(RTMP *r, bool FP9HandShake);
static bool SocksNegotiate(RTMP *r);

static bool SendConnectPacket(RTMP *r);
static bool SendServerBW(RTMP *r);
static bool SendCheckBW(RTMP *r);
static bool SendCheckBWResult(RTMP *r, double txn);
static bool SendCtrl(RTMP *r, short nType, unsigned int nObject, unsigned int nTime);
static bool SendBGHasStream(RTMP *r, double dId, AVal *playpath);
static bool SendCreateStream(RTMP *r, double dStreamId);
static bool SendDeleteStream(RTMP *r, double dStreamId);
static bool SendFCSubscribe(RTMP *r, AVal *subscribepath);
static bool SendPlay(RTMP *r);
static bool SendSeek(RTMP *r, double dTime);
static bool SendBytesReceived(RTMP *r);

static int HandlePacket(RTMP *r, RTMPPacket *packet);
static int HandleInvoke(RTMP *r, const char *body, unsigned int nBodySize);
static bool HandleMetadata(RTMP *r, char *body, unsigned int len);
static void HandleChangeChunkSize(RTMP *r, const RTMPPacket *packet);
static void HandleAudio(RTMP *r, const RTMPPacket *packet);
static void HandleVideo(RTMP *r, const RTMPPacket *packet);
static void HandleCtrl(RTMP *r, const RTMPPacket *packet);
static void HandleServerBW(RTMP *r, const RTMPPacket *packet);
static void HandleClientBW(RTMP *r, const RTMPPacket *packet);

static int EncodeString(char *output, const AVal *name, const AVal *value);
static int EncodeNumber(char *output, const AVal *name, double dVal);
static int EncodeBoolean(char *output, const AVal *name, bool bVal);

static bool SendRTMP(RTMP *r, RTMPPacket *packet, bool queue);

static bool ReadPacket(RTMP *r, RTMPPacket *packet);
static int ReadN(RTMP *r, char *buffer, int n);
static bool WriteN(RTMP *r, const char *buffer, int n);

static bool FillBuffer(RTMP *r);

int32_t RTMP_GetTime()
{
#ifdef _DEBUG
	return 0;
#elif defined(WIN32)
	return timeGetTime();
#else
	struct tms t;
	return times(&t)*1000/sysconf(_SC_CLK_TCK);
#endif
}

void RTMPPacket_Reset(RTMPPacket *p)
{
  p->m_headerType = 0;
  p->m_packetType = 0;
  p->m_nChannel = 0;
  p->m_nInfoField1 = 0; 
  p->m_nInfoField2 = 0; 
  p->m_hasAbsTimestamp = false;
  p->m_nBodySize = 0;
  p->m_nBytesRead = 0;
}

void RTMPPacket_Dump(RTMPPacket *p)
{
  Log(LOGDEBUG,"RTMP PACKET: packet type: 0x%02x. channel: 0x%02x. info 1: %d info 2: %d. Body size: %lu. body: 0x%02x",
	 p->m_packetType, p->m_nChannel,
           p->m_nInfoField1, p->m_nInfoField2, p->m_nBodySize, p->m_body?(unsigned char)p->m_body[0]:0);
}

void RTMP_Init(RTMP *r)
{
  int i;
  for (i=0; i<RTMP_CHANNELS; i++)
  {
    r->m_vecChannelsIn[i] = NULL;
    r->m_vecChannelsOut[i] = NULL;
  }
  RTMP_Close(r);
  r->m_nBufferMS = 300;
  r->m_fDuration = 0;
  r->m_stream_id = -1;
  r->m_pBufferStart = NULL;
  r->m_fAudioCodecs = 3191.0;
  r->m_fVideoCodecs = 252.0;
  r->m_bTimedout = false;
  r->m_pausing = 0;
  r->m_mediaChannel = 0;
}

double RTMP_GetDuration(RTMP *r) { return r->m_fDuration; }
bool RTMP_IsConnected(RTMP *r) { return r->m_socket != 0; }
bool RTMP_IsTimedout(RTMP *r) { return r->m_bTimedout; }

void RTMP_SetBufferMS(RTMP *r, int size)
{
  r->m_nBufferMS = size;
}

void RTMP_UpdateBufferMS(RTMP *r)
{
  SendCtrl(r, 3, r->m_stream_id, r->m_nBufferMS);
}

void RTMP_SetupStream(
	RTMP *r,
	int protocol, 
	const char *hostname, 
	unsigned int port, 
        const char *sockshost,
	AVal *playpath, 
	AVal *tcUrl, 
	AVal *swfUrl, 
	AVal *pageUrl, 
	AVal *app, 
	AVal *auth,
	AVal *swfSHA256Hash,
	uint32_t swfSize,
	AVal *flashVer, 
	AVal *subscribepath, 
	double dTime,
	uint32_t dLength,
	bool bLiveStream,
	long int timeout
)
{
  assert(protocol < 6);

  Log(LOGDEBUG, "Protocol : %s", RTMPProtocolStrings[protocol]);
  Log(LOGDEBUG, "Hostname : %s", hostname);
  Log(LOGDEBUG, "Port     : %d", port);
  Log(LOGDEBUG, "Playpath : %s", playpath->av_val);

  if(tcUrl)
  	Log(LOGDEBUG, "tcUrl    : %s", tcUrl->av_val);
  if(swfUrl)
  	Log(LOGDEBUG, "swfUrl   : %s", swfUrl->av_val);
  if(pageUrl)
  	Log(LOGDEBUG, "pageUrl  : %s", pageUrl->av_val);
  if(app)
  	Log(LOGDEBUG, "app      : %s", app->av_val);
  if(auth)
  	Log(LOGDEBUG, "auth     : %s", auth->av_val);
  if(subscribepath)
  	Log(LOGDEBUG, "subscribepath : %s", subscribepath->av_val);
  if(flashVer)
  	Log(LOGDEBUG, "flashVer : %s", flashVer->av_val);
  if(dTime > 0)
  	Log(LOGDEBUG, "SeekTime      : %.3f sec", (double)dTime/1000.0);
  if(dLength > 0)
  	Log(LOGDEBUG, "playLength    : %.3f sec", (double)dLength/1000.0);

  Log(LOGDEBUG,       "live     : %s", bLiveStream ? "yes":"no");
  Log(LOGDEBUG,       "timeout  : %d sec", timeout);

  if(swfSHA256Hash != NULL && swfSize > 0) {
	r->Link.SWFHash = *swfSHA256Hash;
	r->Link.SWFSize = swfSize;
  	Log(LOGDEBUG, "SWFSHA256:");
  	LogHex(LOGDEBUG, r->Link.SWFHash.av_val, 32);
	Log(LOGDEBUG, "SWFSize  : %lu", r->Link.SWFSize);
  } else {
  	r->Link.SWFHash.av_len = 0;
  	r->Link.SWFHash.av_val = NULL;
	r->Link.SWFSize = 0;
  }

  if(sockshost)
  {
    const char *socksport = strchr(sockshost, ':');
    char *hostname = strdup(sockshost);

    if(socksport)
      hostname[socksport - sockshost] = '\0';
    r->Link.sockshost = hostname;

    r->Link.socksport = socksport ? atoi(socksport + 1) : 1080;
    Log(LOGDEBUG, "Connecting via SOCKS proxy: %s:%d", r->Link.sockshost, r->Link.socksport);
  } else {
    r->Link.sockshost = NULL;
    r->Link.socksport = 0;
  }


  r->Link.tcUrl = *tcUrl;
  r->Link.swfUrl = *swfUrl;
  r->Link.pageUrl = *pageUrl;
  r->Link.app = *app;
  r->Link.auth = *auth;
  r->Link.flashVer = *flashVer;
  r->Link.subscribepath = *subscribepath;
  r->Link.seekTime = dTime;
  r->Link.length = dLength;
  r->Link.bLiveStream = bLiveStream;
  r->Link.timeout = timeout;

  r->Link.protocol = protocol;
  r->Link.hostname = hostname;
  r->Link.port = port;
  r->Link.playpath = *playpath;

  if (r->Link.port == 0)
    r->Link.port = 1935;
}

static bool add_addr_info(struct sockaddr_in* service, const char *hostname, int port)
{
  service->sin_addr.s_addr = inet_addr(hostname);
  if (service->sin_addr.s_addr == INADDR_NONE)
  {
    struct hostent *host = gethostbyname(hostname);
    if (host == NULL || host->h_addr == NULL)
    {
      Log(LOGERROR, "Problem accessing the DNS. (addr: %s)", hostname);
      return false;
    }
    service->sin_addr = *(struct in_addr*)host->h_addr;
  }

  service->sin_port = htons(port);
  return true;
}

bool RTMP_Connect(RTMP *r) {
  struct sockaddr_in service;
  if (!r->Link.hostname)
     return false;

  // close any previous connection
  RTMP_Close(r);

  r->m_bTimedout = false;
  r->m_pausing = 0;
  r->m_fDuration = 0.0;

  memset(&service, 0, sizeof(struct sockaddr_in));
  service.sin_family = AF_INET;

  if (r->Link.socksport)
  {
    // Connect via SOCKS
    if(!add_addr_info(&service, r->Link.sockshost, r->Link.socksport)) return false;
  } else {
    // Connect directly
    if(!add_addr_info(&service, r->Link.hostname, r->Link.port)) return false;
  }

  r->m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (r->m_socket != -1)
  {
    if (connect(r->m_socket, (struct sockaddr*) &service, sizeof(struct sockaddr)) < 0)
    {
      int err = GetSockError();
      Log(LOGERROR, "%s, failed to connect socket. %d (%s)", __FUNCTION__,
	err, strerror(err));
      RTMP_Close(r);
      return false;
    }

    if(r->Link.socksport) {
      Log(LOGDEBUG, "%s ... SOCKS negotiation", __FUNCTION__);
      if (!SocksNegotiate(r))
      {
        Log(LOGERROR, "%s, SOCKS negotiation failed.", __FUNCTION__);
        RTMP_Close(r);
        return false;
      }
    }

    Log(LOGDEBUG, "%s, ... connected, handshaking", __FUNCTION__);
    if (!HandShake(r, true))
    {
      Log(LOGERROR, "%s, handshake failed.", __FUNCTION__);
      RTMP_Close(r);
      return false;
    }

    Log(LOGDEBUG, "%s, handshaked", __FUNCTION__);
    if (!SendConnectPacket(r))
    {
      Log(LOGERROR, "%s, RTMP connect failed.", __FUNCTION__);
      RTMP_Close(r);
      return false;
    }
    // set timeout
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    tv.tv_sec = r->Link.timeout;
    if (setsockopt(r->m_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,  sizeof(tv))) {
      	Log(LOGERROR,"%s, Setting socket timeout to %ds failed!", __FUNCTION__, tv.tv_sec);
    }
  }
  else
  {
    Log(LOGERROR, "%s, failed to create socket. Error: %d", __FUNCTION__, GetSockError());
    return false;
  }

  int on = 1;
  setsockopt(r->m_socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  return true;
}

static bool SocksNegotiate(RTMP *r) {
  struct sockaddr_in service;
  memset(&service, 0, sizeof(struct sockaddr_in));

  add_addr_info(&service, r->Link.hostname, r->Link.port);
  unsigned long addr = htonl(service.sin_addr.s_addr);

  char packet[] = {
      4, 1, // SOCKS 4, connect
      (r->Link.port  >> 8) & 0xFF,
      (r->Link.port) & 0xFF,
      (char) (addr >> 24) & 0xFF, (char) (addr >> 16) & 0xFF,
      (char) (addr >> 8)  & 0xFF, (char) addr & 0xFF,
      0}; // NULL terminate

  WriteN(r, packet, sizeof packet);

  if(ReadN(r, packet, 8) != 8)
    return false;

  if(packet[0] == 0 && packet[1] == 90) {
    return true;
  } else {
    Log(LOGERROR, "%s, SOCKS returned error code %d", packet[1]);
    return false;
  }
}

bool RTMP_ConnectStream(RTMP *r, double seekTime, uint32_t dLength) {
  RTMPPacket packet;
  if (seekTime >= -2.0)
    r->Link.seekTime = seekTime;

  if (dLength >= 0)
    r->Link.length = dLength;

  r->m_mediaChannel = 0;

  RTMPPacket_Reset(&packet);
  while (!r->m_bPlaying && RTMP_IsConnected(r) && ReadPacket(r, &packet)) {
    if (!RTMPPacket_IsReady(&packet))
    {
      continue;
    }
    
    if ((packet.m_packetType == RTMP_PACKET_TYPE_AUDIO) || \
        (packet.m_packetType == RTMP_PACKET_TYPE_VIDEO) || \
        (packet.m_packetType == RTMP_PACKET_TYPE_INFO))
    {
      Log(LOGDEBUG, "%s, received FLV packet before play()!", __FUNCTION__);
      break;
    }

    HandlePacket(r, &packet);
    RTMPPacket_Reset(&packet);
  }

  return r->m_bPlaying;
}

bool RTMP_ReconnectStream(RTMP *r, int bufferTime, double seekTime, uint32_t dLength) {
  RTMP_DeleteStream(r);

  SendCreateStream(r, 2.0);

  RTMP_SetBufferMS(r, bufferTime);

  return RTMP_ConnectStream(r, seekTime, dLength);
}

bool RTMP_ToggleStream(RTMP *r)
{
  bool res;

  res = RTMP_SendPause(r, true, r->m_pauseStamp);
  if (!res) return res;

  r->m_pausing = 1;
  sleep(1);
  res = RTMP_SendPause(r, false, r->m_pauseStamp);
  r->m_pausing = 3;
  return res;
}

void RTMP_DeleteStream(RTMP *r) {
  if (r->m_stream_id < 0)
    return;

  r->m_bPlaying = false;

  SendDeleteStream(r, r->m_stream_id);
}

int RTMP_GetNextMediaPacket(RTMP *r, RTMPPacket *packet)
{
  int bHasMediaPacket = 0;

  RTMPPacket_Reset(packet);
  while (!bHasMediaPacket && RTMP_IsConnected(r) && ReadPacket(r, packet))
  {
    if (!RTMPPacket_IsReady(packet))
    {
      continue;
    }

    bHasMediaPacket = HandlePacket(r, packet);

    if (!bHasMediaPacket) { 
      RTMPPacket_Reset(packet);
    } else if (r->m_pausing == 3) {
      if (packet->m_nTimeStamp <= r->m_mediaStamp) {
	bHasMediaPacket = 0;
#ifdef _DEBUG
	Log(LOGDEBUG, "Skipped type: %02X, size: %d, TS: %d ms, abs TS: %d, pause: %d ms",
	packet->m_packetType, packet->m_nBodySize, packet->m_nTimeStamp, packet->m_hasAbsTimestamp, r->m_mediaStamp);
#endif
	continue;
      }
      r->m_pausing = 0;
    }
  }
        
  if (bHasMediaPacket)
    r->m_bPlaying = true;
  else if (r->m_bTimedout)
    r->m_pauseStamp = r->m_channelTimestamp[r->m_mediaChannel];

  return bHasMediaPacket;
}

static int HandlePacket(RTMP *r, RTMPPacket *packet) {
  int bHasMediaPacket = 0;
    switch (packet->m_packetType)
    {
      case 0x01:
        // chunk size
        HandleChangeChunkSize(r, packet);
        break;

      case 0x03:
        // bytes read report
        Log(LOGDEBUG, "%s, received: bytes read report", __FUNCTION__);
        break;

      case 0x04:
        // ctrl
        HandleCtrl(r, packet);
        break;

      case 0x05:
        // server bw
	HandleServerBW(r, packet);
        break;

      case 0x06:
        // client bw
	HandleClientBW(r, packet);
        break;

      case 0x08:
        // audio data
        //Log(LOGDEBUG, "%s, received: audio %lu bytes", __FUNCTION__, packet.m_nBodySize);
        HandleAudio(r, packet);
        bHasMediaPacket = 1;
	if (!r->m_mediaChannel)
	  r->m_mediaChannel = packet->m_nChannel;
	if (!r->m_pausing)
	  r->m_mediaStamp = packet->m_nTimeStamp;
        break;

      case 0x09:
        // video data
        //Log(LOGDEBUG, "%s, received: video %lu bytes", __FUNCTION__, packet.m_nBodySize);
        HandleVideo(r, packet);
        bHasMediaPacket = 1;
	if (!r->m_mediaChannel)
	  r->m_mediaChannel = packet->m_nChannel;
	if (!r->m_pausing)
	  r->m_mediaStamp = packet->m_nTimeStamp;
        break;

      case 0x0F: // flex stream send
        Log(LOGDEBUG, "%s, flex stream send, size %lu bytes, not supported, ignoring", __FUNCTION__, packet->m_nBodySize);
	break;

      case 0x10: // flex shared object
        Log(LOGDEBUG, "%s, flex shared object, size %lu bytes, not supported, ignoring", __FUNCTION__, packet->m_nBodySize);
	break;

      case 0x11: // flex message
      {
        Log(LOGDEBUG, "%s, flex message, size %lu bytes, not fully supported", __FUNCTION__, packet->m_nBodySize);
	//LogHex(packet.m_body, packet.m_nBodySize);

	// some DEBUG code
	/*RTMP_LIB_AMFObject obj;
        int nRes = obj.Decode(packet.m_body+1, packet.m_nBodySize-1);
        if(nRes < 0) {
                Log(LOGERROR, "%s, error decoding AMF3 packet", __FUNCTION__);
                //return;
        }

        obj.Dump();*/

	if ( HandleInvoke(r, packet->m_body+1, packet->m_nBodySize-1) == 1 )
	  bHasMediaPacket = 2;
	break;
      }
      case 0x12:
        // metadata (notify)
        Log(LOGDEBUG, "%s, received: notify %lu bytes", __FUNCTION__, packet->m_nBodySize);
        if ( HandleMetadata(r, packet->m_body, packet->m_nBodySize) )
          bHasMediaPacket = 1;
        break;

      case 0x13:
      	Log(LOGDEBUG, "%s, shared object, not supported, ignoring", __FUNCTION__);
	break;

      case 0x14:
        // invoke
	Log(LOGDEBUG, "%s, received: invoke %lu bytes", __FUNCTION__, packet->m_nBodySize);
        //LogHex(packet.m_body, packet.m_nBodySize);

	if ( HandleInvoke(r, packet->m_body, packet->m_nBodySize) == 1 )
		bHasMediaPacket = 2;
        break;

      case 0x16:
      {
	// go through FLV packets and handle metadata packets
        unsigned int pos=0;
	uint32_t nTimeStamp = packet->m_nTimeStamp;

	Log(LOGDEBUG, "%s, received: FLV %lu bytes", __FUNCTION__, packet->m_nBodySize);

        while(pos+11 < packet->m_nBodySize) {
		uint32_t dataSize = AMF_DecodeInt24(packet->m_body+pos+1); // size without header (11) and prevTagSize (4)

                if(pos+11+dataSize+4 > packet->m_nBodySize) {
                        Log(LOGWARNING, "Stream corrupt?!");
                	break;
                }
		if(packet->m_body[pos] == 0x12) {
			HandleMetadata(r, packet->m_body+pos+11, dataSize);
		} else if (packet->m_body[pos] == 8 || packet->m_body[pos] == 9) {
			nTimeStamp = AMF_DecodeInt24(packet->m_body+pos+4);
			nTimeStamp |= (packet->m_body[pos+7]<<24);
		}
                pos += (11+dataSize+4);
	}
	if (!r->m_pausing)
	  r->m_mediaStamp = nTimeStamp;

        // FLV tag(s)
        //Log(LOGDEBUG, "%s, received: FLV tag(s) %lu bytes", __FUNCTION__, packet.m_nBodySize);
        bHasMediaPacket = 1;
        break;
      }
      default:
        Log(LOGDEBUG, "%s, unknown packet type received: 0x%02x", __FUNCTION__, packet->m_packetType);
	#ifdef _DEBUG
	LogHex(LOGDEBUG, packet->m_body, packet->m_nBodySize);
	#endif
    }

  Log(LOGDEBUG, "%s, done %lu bytes", __FUNCTION__, packet->m_nBodySize);
  return bHasMediaPacket;
}

#ifdef _DEBUG
extern FILE *netstackdump;
extern FILE *netstackdump_read;
#endif

static int ReadN(RTMP *r, char *buffer, int n)
{
  int nOriginalSize = n;
  char *ptr;
  
  r->m_bTimedout = false;

#ifdef _DEBUG
  memset(buffer, 0, n);
#endif

  ptr = buffer;
  while (n > 0)
  {
    int nBytes = 0, nRead;
    if(r->m_nBufferSize == 0)
	if (!FillBuffer(r)) {
	   if (!r->m_bTimedout)
	     RTMP_Close(r);
	   return 0;
	}
    nRead = ((n<r->m_nBufferSize)?n:r->m_nBufferSize);
    if(nRead > 0) {
    	memcpy(ptr, r->m_pBufferStart, nRead);
	r->m_pBufferStart += nRead;
	r->m_nBufferSize -= nRead;
	nBytes = nRead;
	r->m_nBytesIn += nRead;
	if(r->m_nBytesIn > r->m_nBytesInSent + r->m_nClientBW/2 )
		SendBytesReceived(r);
    }

    //Log(LOGDEBUG, "%s: %d bytes\n", __FUNCTION__, nBytes);
#ifdef _DEBUG
        fwrite(ptr, 1, nBytes, netstackdump_read);
#endif

    if (nBytes == 0)
    {
      Log(LOGDEBUG, "%s, RTMP socket closed by server", __FUNCTION__);
      //goto again;
      RTMP_Close(r);
      break;
    }
  
#ifdef CRYPTO
    if(r->Link.rc4keyIn) {
    	RC4(r->Link.rc4keyIn, nBytes, (uint8_t*)ptr, (uint8_t*)ptr);
    }
#endif

    n -= nBytes;
    ptr += nBytes;
  }

  return nOriginalSize - n;
}

static bool WriteN(RTMP *r, const char *buffer, int n)
{
  const char *ptr = buffer;
#ifdef CRYPTO
  char *encrypted = 0;
  char buf[RTMP_BUFFER_CACHE_SIZE];
 
  if(r->Link.rc4keyOut) {
    if (n > sizeof(buf))
      encrypted = (char *)malloc(n);
    else
      encrypted = (char *)buf;
    ptr = encrypted;
    RC4(r->Link.rc4keyOut, n, (uint8_t*)buffer, (uint8_t*)ptr);
  }
#endif
  
  while (n > 0)
  {
#ifdef _DEBUG
	fwrite(ptr, 1, n, netstackdump);
#endif

    int nBytes = send(r->m_socket, ptr, n, 0);
    //Log(LOGDEBUG, "%s: %d\n", __FUNCTION__, nBytes);
    
    if (nBytes < 0)
    {
      int sockerr = GetSockError();
      Log(LOGERROR, "%s, RTMP send error %d (%d bytes)", __FUNCTION__, sockerr, n);

      if (sockerr == EINTR && !bCtrlC)
	continue;
      
      RTMP_Close(r);
      n = 1;
      break;
    }
    
    if (nBytes == 0)
      break;
    
    n -= nBytes;
    ptr += nBytes;
  }

#ifdef CRYPTO
  if(encrypted && encrypted != buf)
    free(encrypted);
#endif

  return n == 0;
}

#define SAVC(x)	static const AVal av_##x = AVC(#x)

SAVC(app);
SAVC(connect);
SAVC(flashVer);
SAVC(swfUrl);
SAVC(pageUrl);
SAVC(tcUrl);
SAVC(fpad);
SAVC(capabilities);
SAVC(audioCodecs);
SAVC(videoCodecs);
SAVC(videoFunction);
SAVC(objectEncoding);

static bool SendConnectPacket(RTMP *r)
{
  RTMPPacket packet;

  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_connect);
  enc += AMF_EncodeNumber(enc, 1.0);
  *enc++ = AMF_OBJECT;
 
  if(r->Link.app.av_len)
  	enc += EncodeString(enc, &av_app, &r->Link.app);
  if(r->Link.flashVer.av_len)
  	enc += EncodeString(enc, &av_flashVer, &r->Link.flashVer);
  if(r->Link.swfUrl.av_len)
 	enc += EncodeString(enc, &av_swfUrl, &r->Link.swfUrl);
  if(r->Link.tcUrl.av_len)
  	enc += EncodeString(enc, &av_tcUrl, &r->Link.tcUrl);
  
  enc += EncodeBoolean(enc, &av_fpad, false);
  enc += EncodeNumber(enc, &av_capabilities, 15.0);
  enc += EncodeNumber(enc, &av_audioCodecs, r->m_fAudioCodecs);
  enc += EncodeNumber(enc, &av_videoCodecs, r->m_fVideoCodecs);
  enc += EncodeNumber(enc, &av_videoFunction, 1.0);
  if(r->Link.pageUrl.av_len)
  	enc += EncodeString(enc, &av_pageUrl, &r->Link.pageUrl);

  enc += EncodeNumber(enc, &av_objectEncoding, 0.0); // AMF0, AMF3 not supported yet
  *enc++ = 0; *enc++ = 0; // end of object - 0x00 0x00 0x09
  *enc++ = AMF_OBJECT_END;
 
  // add auth string
  if(r->Link.auth.av_len)
  {
  	*enc++ = 0x01;
  	*enc++ = 0x01;

  	enc += AMF_EncodeString(enc, &r->Link.auth);
  }
  packet.m_nBodySize = enc-packet.m_body;

  return SendRTMP(r, &packet, true);
}

SAVC(bgHasStream);

static bool SendBGHasStream(RTMP *r, double dId, AVal *playpath)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_bgHasStream);
  enc += AMF_EncodeNumber(enc, dId);
  *enc++ = AMF_NULL;

  enc += AMF_EncodeString(enc, playpath);

  packet.m_nBodySize = enc-packet.m_body;

  return SendRTMP(r, &packet, true);
}

SAVC(createStream);

static bool SendCreateStream(RTMP *r, double dStreamId)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_createStream);
  enc += AMF_EncodeNumber(enc, dStreamId);
  *enc++ = AMF_NULL; // NULL

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, true);
}

SAVC(FCSubscribe);

static bool SendFCSubscribe(RTMP *r, AVal *subscribepath)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  Log(LOGDEBUG, "FCSubscribe: %s", subscribepath);
  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_FCSubscribe);
  enc += AMF_EncodeNumber(enc, 4.0);
  *enc++ = AMF_NULL;
  enc += AMF_EncodeString(enc, subscribepath);

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, true);
}

SAVC(deleteStream);

static bool SendDeleteStream(RTMP *r, double dStreamId)
{
  RTMPPacket packet;

  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_deleteStream);
  enc += AMF_EncodeNumber(enc, 0.0);
  *enc++ = AMF_NULL;
  enc += AMF_EncodeNumber(enc, dStreamId);

  packet.m_nBodySize = enc - packet.m_body;

  /* no response expected */
  return SendRTMP(r, &packet, false);
}

SAVC(pause);

bool RTMP_SendPause(RTMP *r, bool DoPause, double dTime)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x08;   // video channel 
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // invoke
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_pause);
  enc += AMF_EncodeNumber(enc, 0);
  *enc++ = AMF_NULL;
  enc += AMF_EncodeBoolean(enc, DoPause);
  enc += AMF_EncodeNumber(enc, (double)dTime);

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, true);
}

SAVC(seek);

static bool SendSeek(RTMP *r, double dTime)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x08;   // video channel 
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // invoke
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_seek);
  enc += AMF_EncodeNumber(enc, 0);
  *enc++ = AMF_NULL;
  enc += AMF_EncodeNumber(enc, dTime);

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, true);
}

static bool SendServerBW(RTMP *r)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x02;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
  packet.m_packetType = 0x05; // Server BW
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  packet.m_nBodySize = 4;

  AMF_EncodeInt32(packet.m_body, r->m_nServerBW);
  return SendRTMP(r, &packet, false);
}

static bool SendBytesReceived(RTMP *r)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x02;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x03; // bytes in
  packet.m_nInfoField1 = 0;
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  packet.m_nBodySize = 4;

  AMF_EncodeInt32(packet.m_body, r->m_nBytesIn); // hard coded for now
  r->m_nBytesInSent = r->m_nBytesIn;

  //Log(LOGDEBUG, "Send bytes report. 0x%x (%d bytes)", (unsigned int)m_nBytesIn, m_nBytesIn);
  return SendRTMP(r, &packet, false);
}

SAVC(_checkbw);

static bool SendCheckBW(RTMP *r)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = RTMP_GetTime();
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av__checkbw);
  enc += AMF_EncodeNumber(enc, 0);
  *enc++ = AMF_NULL;

  packet.m_nBodySize = enc - packet.m_body;

  // triggers _onbwcheck and eventually results in _onbwdone 
  return SendRTMP(r, &packet, false);
}

SAVC(_result);

static bool SendCheckBWResult(RTMP *r, double txn)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x03;   // control channel (invoke)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField1 = 0x16 * r->m_nBWCheckCounter; // temp inc value. till we figure it out.
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av__result);
  enc += AMF_EncodeNumber(enc, txn);
  *enc++ = AMF_NULL;
  enc += AMF_EncodeNumber(enc, (double)r->m_nBWCheckCounter++); 

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, false);
}

SAVC(play);

static bool SendPlay(RTMP *r)
{
  RTMPPacket packet;
  packet.m_nChannel = 0x08;   // we make 8 our stream channel
  packet.m_headerType = RTMP_PACKET_SIZE_LARGE;
  packet.m_packetType = 0x14; // INVOKE
  packet.m_nInfoField2 = r->m_stream_id; //0x01000000;
  packet.m_nInfoField1 = 0;
  packet.m_hasAbsTimestamp = 0;

  char *enc = packet.m_body;
  enc += AMF_EncodeString(enc, &av_play);
  enc += AMF_EncodeNumber(enc, 0.0); // stream id??
  *enc++ = AMF_NULL;

  Log(LOGDEBUG, "%s, seekTime=%.2f, dLength=%d, sending play: %s", __FUNCTION__, r->Link.seekTime, r->Link.length, r->Link.playpath.av_val);
  enc += AMF_EncodeString(enc, &r->Link.playpath);

  // Optional parameters start and len.

  // start: -2, -1, 0, positive number
  //  -2: looks for a live stream, then a recorded stream, if not found any open a live stream
  //  -1: plays a live stream
  // >=0: plays a recorded streams from 'start' milliseconds
  if(r->Link.bLiveStream)
    enc += AMF_EncodeNumber(enc, -1000.0);
  else {
  if(r->Link.seekTime > 0.0)
    enc += AMF_EncodeNumber(enc, r->Link.seekTime); // resume from here
    else
      enc += AMF_EncodeNumber(enc, 0.0);//-2000.0); // recorded as default, -2000.0 is not reliable since that freezes the player if the stream is not found
  }
  
  // len: -1, 0, positive number
  //  -1: plays live or recorded stream to the end (default)
  //   0: plays a frame 'start' ms away from the beginning
  //  >0: plays a live or recoded stream for 'len' milliseconds
  //enc += EncodeNumber(enc, -1.0); // len
  if(r->Link.length)
    enc += AMF_EncodeNumber(enc, r->Link.length); // len

  packet.m_nBodySize = enc - packet.m_body;

  return SendRTMP(r, &packet, true);
}
/*
from http://jira.red5.org/confluence/display/docs/Ping:

Ping is the most mysterious message in RTMP and till now we haven't fully interpreted it yet. In summary, Ping message is used as a special command that are exchanged between client and server. This page aims to document all known Ping messages. Expect the list to grow.

The type of Ping packet is 0x4 and contains two mandatory parameters and two optional parameters. The first parameter is the type of Ping and in short integer. The second parameter is the target of the ping. As Ping is always sent in Channel 2 (control channel) and the target object in RTMP header is always 0 which means the Connection object, it's necessary to put an extra parameter to indicate the exact target object the Ping is sent to. The second parameter takes this responsibility. The value has the same meaning as the target object field in RTMP header. (The second value could also be used as other purposes, like RTT Ping/Pong. It is used as the timestamp.) The third and fourth parameters are optional and could be looked upon as the parameter of the Ping packet. Below is an unexhausted list of Ping messages.

    * type 0: Clear the stream. No third and fourth parameters. The second parameter could be 0. After the connection is established, a Ping 0,0 will be sent from server to client. The message will also be sent to client on the start of Play and in response of a Seek or Pause/Resume request. This Ping tells client to re-calibrate the clock with the timestamp of the next packet server sends.
    * type 1: Tell the stream to clear the playing buffer.
    * type 3: Buffer time of the client. The third parameter is the buffer time in millisecond.
    * type 4: Reset a stream. Used together with type 0 in the case of VOD. Often sent before type 0.
    * type 6: Ping the client from server. The second parameter is the current time.
    * type 7: Pong reply from client. The second parameter is the time the server sent with his ping request.
    * type 26: SWFVerification request
    * type 27: SWFVerification response
*/
static bool SendCtrl(RTMP *r, short nType, unsigned int nObject, unsigned int nTime)
{
  Log(LOGDEBUG, "sending ctrl. type: 0x%04x", (unsigned short)nType);

  RTMPPacket packet; 
  packet.m_nChannel = 0x02;   // control channel (ping)
  packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;
  packet.m_packetType = 0x04; // ctrl
  packet.m_nInfoField1 = RTMP_GetTime();
  packet.m_nInfoField2 = 0;
  packet.m_hasAbsTimestamp = 0;

  int nSize = (nType==0x03?10:6); // type 3 is the buffer time and requires all 3 parameters. all in all 10 bytes.
  if(nType == 0x1B) 
    nSize = 44;

  packet.m_nBodySize = nSize;

  char *buf = packet.m_body;
  buf += AMF_EncodeInt16(buf, nType);

  if(nType == 0x1B) {
#ifdef CRYPTO
    memcpy(buf, r->Link.SWFVerificationResponse, 42);
    Log(LOGDEBUG, "Sending SWFVerification response: ");
    LogHex(LOGDEBUG, packet.m_body, packet.m_nBodySize);
#endif
  } else {
    if (nSize > 2)
      buf += AMF_EncodeInt32(buf, nObject);

    if (nSize > 6)
      buf += AMF_EncodeInt32(buf, nTime);
  }
  
  return SendRTMP(r, &packet, false);
}

static void AV_erase(AVal *vals, int *num, int i, bool freeit)
{
  if (freeit)
    free(vals[i].av_val);
  (*num)--;
  for (; i<*num; i++) {
    vals[i] = vals[i+1];
  }
  vals[i].av_val = NULL;
  vals[i].av_len = 0;
}

static void AV_queue(AVal **vals, int *num, AVal *av)
{
  char *tmp;
  if (!(*num & 0x0f))
    *vals = realloc(*vals, (*num+16) * sizeof(AVal));
  tmp = malloc(av->av_len+1);
  memcpy(tmp, av->av_val, av->av_len);
  tmp[av->av_len] = '\0';
  (*vals)[*num].av_len = av->av_len;
  (*vals)[(*num)++].av_val = tmp;
}

static void AV_clear(AVal *vals, int num)
{
  int i;
  for (i=0;i<num;i++)
    free(vals[i].av_val);
  free(vals);
}

SAVC(onBWDone);
SAVC(onFCSubscribe);
SAVC(onFCUnsubscribe);
SAVC(_onbwcheck);
SAVC(_onbwdone);
SAVC(_error);
SAVC(close);
SAVC(code);
SAVC(level);
SAVC(onStatus);
static const AVal av_NetStream_Failed = AVC("NetStream.Failed");
static const AVal av_NetStream_Play_Failed = AVC("NetStream.Play.Failed");
static const AVal av_NetStream_Play_StreamNotFound = AVC("NetStream.Play.StreamNotFound");
static const AVal av_NetConnection_Connect_InvalidApp = AVC("NetConnection.Connect.InvalidApp");
static const AVal av_NetStream_Play_Start = AVC("NetStream.Play.Start");
static const AVal av_NetStream_Play_Complete = AVC("NetStream.Play.Complete");
static const AVal av_NetStream_Play_Stop = AVC("NetStream.Play.Stop");

// Returns 0 for OK/Failed/error, 1 for 'Stop or Complete'
static int HandleInvoke(RTMP *r, const char *body, unsigned int nBodySize)
{
  int ret = 0, nRes;
  if (body[0] != 0x02) // make sure it is a string method name we start with
  {
    Log(LOGWARNING, "%s, Sanity failed. no string method in invoke packet", __FUNCTION__);
    return 0;
  }

  AMFObject obj;
  nRes = AMF_Decode(&obj, body, nBodySize, false);
  if (nRes < 0)
  { 
    Log(LOGERROR, "%s, error decoding invoke packet", __FUNCTION__);
    return 0;
  }

  AMF_Dump(&obj);
  AVal method;
  AMFProp_GetString(AMF_GetProp(&obj, NULL, 0), &method);
  double txn = AMFProp_GetNumber(AMF_GetProp(&obj, NULL, 1));
  Log(LOGDEBUG, "%s, server invoking <%s>", __FUNCTION__, method.av_val);

  if (AVMATCH(&method, &av__result))
  {
    AVal methodInvoked = r->m_methodCalls[0];
    AV_erase(r->m_methodCalls, &r->m_numCalls, 0, false);

    Log(LOGDEBUG, "%s, received result for method call <%s>", __FUNCTION__, methodInvoked.av_val);
  
    if (AVMATCH(&methodInvoked,&av_connect))
    {
      SendServerBW(r);
      SendCtrl(r, 3, 0, 300);

      SendCreateStream(r, 2.0);

      // Send the FCSubscribe if live stream or if subscribepath is set
      if (r->Link.subscribepath.av_len)
        SendFCSubscribe(r, &r->Link.subscribepath);
      else if (r->Link.bLiveStream)
        SendFCSubscribe(r, &r->Link.playpath);
    }
    else if (AVMATCH(&methodInvoked,&av_createStream))
    {
      r->m_stream_id = (int)AMFProp_GetNumber(AMF_GetProp(&obj,NULL,3));

      SendPlay(r);
      SendCtrl(r, 3, r->m_stream_id, r->m_nBufferMS);
    }
    else if (AVMATCH(&methodInvoked,&av_play))
    {
	r->m_bPlaying = true;
    }
    free(methodInvoked.av_val);
  }
  else if (AVMATCH(&method,&av_onBWDone))
  {
    SendCheckBW(r);
  }
  else if (AVMATCH(&method,&av_onFCSubscribe))
  {
    // SendOnFCSubscribe();
  }
  else if (AVMATCH(&method,&av_onFCUnsubscribe))
  {
    RTMP_Close(r);
    ret = 1;
  }
  else if (AVMATCH(&method,&av__onbwcheck))
  {
    SendCheckBWResult(r, txn);
  }
  else if (AVMATCH(&method,&av__onbwdone))
  {
    int i;
    for (i=0; i<r->m_numCalls; i++)
      if (AVMATCH(&r->m_methodCalls[i],&av__checkbw)) {
        AV_erase(r->m_methodCalls, &r->m_numCalls, i, true);
        break;
      }
  }
  else if (AVMATCH(&method,&av__error))
  {
    Log(LOGERROR, "rtmp server sent error");
  }
  else if (AVMATCH(&method,&av_close))
  {
    Log(LOGERROR, "rtmp server requested close");
    RTMP_Close(r);
  }
  else if (AVMATCH(&method,&av_onStatus))
  {
    AMFObject obj2;
    AVal code, level;
    AMFProp_GetObject(AMF_GetProp(&obj, NULL, 3), &obj2);
    AMFProp_GetString(AMF_GetProp(&obj2, &av_code, -1), &code);
    AMFProp_GetString(AMF_GetProp(&obj2, &av_level, -1), &level);

    Log(LOGDEBUG, "%s, onStatus: %s", __FUNCTION__, code.av_val );
    if (AVMATCH(&code,&av_NetStream_Failed)
    ||  AVMATCH(&code,&av_NetStream_Play_Failed)
    ||  AVMATCH(&code,&av_NetStream_Play_StreamNotFound)
    ||  AVMATCH(&code,&av_NetConnection_Connect_InvalidApp)) {
      r->m_stream_id = -1;
      RTMP_Close(r);
    }

    if (AVMATCH(&code, &av_NetStream_Play_Start)) {
      int i;
      r->m_bPlaying = true;
      for (i=0; i<r->m_numCalls; i++) {
        if (AVMATCH(&r->m_methodCalls[i], &av_play)) {
          AV_erase(r->m_methodCalls, &r->m_numCalls, i, true);
          break;
        }
      }
    }

    // Return 1 if this is a Play.Complete or Play.Stop
    if (AVMATCH(&code,&av_NetStream_Play_Complete)
    ||  AVMATCH(&code,&av_NetStream_Play_Stop)) {
      RTMP_Close(r);
      ret = 1;
    }
  }
  else
  {

  }
  AMF_Reset(&obj);
  return ret;
}

bool RTMP_FindFirstMatchingProperty(AMFObject *obj, const AVal *name, AMFObjectProperty *p)
{
	int n;
	/* this is a small object search to locate the "duration" property */
	for (n=0; n<obj->o_num; n++) {
		AMFObjectProperty *prop = AMF_GetProp(obj, NULL, n);

		if(AVMATCH(&prop->p_name, name)) {
			*p = *prop;
			return true;
		}

		if(prop->p_type == AMF_OBJECT) {
			return RTMP_FindFirstMatchingProperty(&prop->p_vu.p_object, name, p);
		}
	}
	return false;
}

static bool DumpMetaData(AMFObject *obj)
{
        AMFObjectProperty *prop;
	int n;
	for (n=0; n<obj->o_num; n++) {
		prop = AMF_GetProp(obj, NULL, n);
		if ( prop->p_type != AMF_OBJECT ) {
			char str[256]="";
			switch( prop->p_type )
			{
				case AMF_NUMBER:
					snprintf(str, 255, "%.2f", prop->p_vu.p_number);
					break;
				case AMF_BOOLEAN:
					snprintf(str, 255, "%s", prop->p_vu.p_number != 0.?"TRUE":"FALSE");
					break;
				case AMF_STRING:
					snprintf(str, 255, "%.*s", prop->p_vu.p_aval.av_len, prop->p_vu.p_aval.av_val);
					break;
				case AMF_DATE:
					snprintf(str, 255, "timestamp:%.2f", prop->p_vu.p_number);
					break;
				default:
					snprintf(str, 255, "INVALID TYPE 0x%02x", (unsigned char)prop->p_type );
			}
			if ( prop->p_name.av_len ) {
				// chomp
				if ( strlen(str) >= 1 && str[strlen(str)-1 ] == '\n')
					str[strlen(str)-1] = '\0';
				LogPrintf("  %-22.*s%s\n", prop->p_name.av_len, prop->p_name.av_val, str );
			}
		} else {
			if ( prop->p_name.av_len )
				LogPrintf("%.*s:\n", prop->p_name.av_len, prop->p_name.av_val );
			DumpMetaData(&prop->p_vu.p_object);
		}
	}
	return false;
}

SAVC(onMetaData);
SAVC(duration);

static bool HandleMetadata(RTMP *r, char *body, unsigned int len)
{
	// allright we get some info here, so parse it and print it
	// also keep duration or filesize to make a nice progress bar

	AMFObject obj;
        AVal metastring;
	bool ret = false;

	int nRes = AMF_Decode(&obj, body, len, false);
	if(nRes < 0) {
		Log(LOGERROR, "%s, error decoding meta data packet", __FUNCTION__);
		return false;
	}

	AMF_Dump(&obj);
	AMFProp_GetString(AMF_GetProp(&obj, NULL, 0), &metastring);

	if(AVMATCH(&metastring, &av_onMetaData)) {
		AMFObjectProperty prop;
		// Show metadata
		LogPrintf("\r%s\n", "Metadata:                  " );
		DumpMetaData(&obj);
		if(RTMP_FindFirstMatchingProperty(&obj, &av_duration, &prop)) {
			r->m_fDuration = prop.p_vu.p_number;
			//Log(LOGDEBUG, "Set duration: %.2f", m_fDuration);
		}
		ret = true;
	}
	AMF_Reset(&obj);
	return ret;
}

static void HandleChangeChunkSize(RTMP *r, const RTMPPacket *packet)
{
  if (packet->m_nBodySize >= 4)
  {
    r->m_chunkSize = AMF_DecodeInt32(packet->m_body);
    Log(LOGDEBUG, "%s, received: chunk size change to %d", __FUNCTION__, r->m_chunkSize);
  }
}

static void HandleAudio(RTMP *r, const RTMPPacket *packet)
{
}

static void HandleVideo(RTMP *r, const RTMPPacket *packet)
{
}

static void HandleCtrl(RTMP *r, const RTMPPacket *packet)
{
  short nType = -1;
  unsigned int tmp;
  if (packet->m_body && packet->m_nBodySize >= 2)
    nType = AMF_DecodeInt16(packet->m_body);
  Log(LOGDEBUG, "%s, received ctrl. type: %d, len: %d", __FUNCTION__, nType, packet->m_nBodySize);
  //LogHex(packet.m_body, packet.m_nBodySize);

  if (packet->m_nBodySize >= 6) {
    switch(nType) {
    case 0:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream Begin %d", __FUNCTION__, tmp);
      break;

    case 1:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream EOF %d", __FUNCTION__, tmp);
      if (r->m_pausing == 1)
        r->m_pausing = 2;
      break;

    case 2:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream Dry %d", __FUNCTION__, tmp);
      break;

    case 4:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream IsRecorded %d", __FUNCTION__, tmp);
      break;

    case 6: // server ping. reply with pong.
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Ping %d", __FUNCTION__, tmp);
      SendCtrl(r, 0x07, tmp, 0);
      break;

    case 31:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream BufferEmpty %d", __FUNCTION__, tmp);
      if (!r->m_pausing) {
	r->m_pauseStamp = r->m_channelTimestamp[r->m_mediaChannel];
        RTMP_SendPause(r, true, r->m_pauseStamp);
        r->m_pausing = 1;
      } else if (r->m_pausing == 2) {
        RTMP_SendPause(r, false, r->m_pauseStamp);
        r->m_pausing = 3;
      }
      break;

    case 32:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream BufferReady %d", __FUNCTION__, tmp);
      break;

    default:
      tmp = AMF_DecodeInt32(packet->m_body + 2);
      Log(LOGDEBUG, "%s, Stream xx %d", __FUNCTION__, tmp);
      break;
    }

  }

  if (nType == 0x1A) {
  	Log(LOGDEBUG, "%s, SWFVerification ping received: ", __FUNCTION__);
	//LogHex(packet.m_body, packet.m_nBodySize);

	// respond with HMAC SHA256 of decompressed SWF, key is the 30byte player key, also the last 30 bytes of the server handshake are applied
	if(r->Link.SWFHash.av_len) {
	  SendCtrl(r, 0x1B, 0, 0);
	} else {
	  Log(LOGWARNING, "%s: Ignoring SWFVerification request, use --swfhash and --swfsize!", __FUNCTION__);
	}
  }
}

static void HandleServerBW(RTMP *r, const RTMPPacket *packet) {
  r->m_nServerBW = AMF_DecodeInt32(packet->m_body);
  Log(LOGDEBUG, "%s: server BW = %d", __FUNCTION__, r->m_nServerBW);
}

static void HandleClientBW(RTMP *r, const RTMPPacket *packet) {
  r->m_nClientBW = AMF_DecodeInt32(packet->m_body);
  if (packet->m_nBodySize > 4)
    r->m_nClientBW2 = packet->m_body[4];
  else
    r->m_nClientBW2 = -1;
  Log(LOGDEBUG, "%s: client BW = %d %d", __FUNCTION__, r->m_nClientBW, r->m_nClientBW2);
}

static bool ReadPacket(RTMP *r, RTMPPacket *packet)
{
  char type;
  if (ReadN(r, &type,1) == 0)
  {
    Log(LOGERROR, "%s, failed to read RTMP packet header", __FUNCTION__);
    return false;
  } 

  packet->m_headerType = (type & 0xc0) >> 6;
  packet->m_nChannel = (type & 0x3f);
  if ( packet->m_nChannel == 0 )
  {
	if (ReadN(r,&type,1) != 1)
	{
	  Log(LOGERROR, "%s, failed to read RTMP packet header 2nd byte", __FUNCTION__);
	  return false;
	} 
	packet->m_nChannel = (unsigned)type;
	packet->m_nChannel += 64;
  } else if ( packet->m_nChannel == 1 )
  {
    char t[2];
	int tmp;
    if (ReadN(r,t,2) != 2)
	{
	  Log(LOGERROR, "%s, failed to read RTMP packet header 3nd byte", __FUNCTION__);
	  return false;
	} 
	tmp = (((unsigned)t[1])<<8) + (unsigned)t[0];
	packet->m_nChannel = tmp + 64;
    Log(LOGDEBUG, "%s, m_nChannel: %0x", __FUNCTION__, packet->m_nChannel);
  }

  int nSize = packetSize[packet->m_headerType];
  
  if (nSize == RTMP_LARGE_HEADER_SIZE) // if we get a full header the timestamp is absolute
    packet->m_hasAbsTimestamp = true; 

  if (nSize < RTMP_LARGE_HEADER_SIZE) { // using values from the last message of this channel
	if (r->m_vecChannelsIn[packet->m_nChannel])
		memcpy(packet, r->m_vecChannelsIn[packet->m_nChannel], offsetof(RTMPPacket,m_header));
  }
  
  nSize--;

  char *header = packet->m_header;
  if (nSize > 0 && ReadN(r, header,nSize) != nSize)
  {
    Log(LOGERROR, "%s, failed to read RTMP packet header. type: %x", __FUNCTION__, (unsigned int)type);
    return false;
  }

  if (nSize >= 3)
    packet->m_nInfoField1 = AMF_DecodeInt24(header);

  //Log(LOGDEBUG, "%s, reading RTMP packet chunk on channel %x, headersz %i, timestamp %i, abs timestamp %i", __FUNCTION__, packet.m_nChannel, nSize, packet.m_nInfoField1, packet.m_hasAbsTimestamp); 

  if (nSize >= 6)
  {
    packet->m_nBodySize = AMF_DecodeInt24(header + 3);
    packet->m_nBytesRead = 0;
  }
  
  if (nSize > 6)
    packet->m_packetType = header[6];

  if (nSize == 11)
    packet->m_nInfoField2 = ReadInt32LE(header+7);

  int nToRead = packet->m_nBodySize - packet->m_nBytesRead;
  int nChunk = r->m_chunkSize;
  if (nToRead < nChunk)
     nChunk = nToRead;

  if (ReadN(r, packet->m_body + packet->m_nBytesRead, nChunk) != nChunk)
  {
    Log(LOGERROR, "%s, failed to read RTMP packet body. len: %lu", __FUNCTION__, packet->m_nBodySize);
    return false;  
  }

  packet->m_nBytesRead += nChunk;

  // keep the packet as ref for other packets on this channel
  if (!r->m_vecChannelsIn[packet->m_nChannel])
  	r->m_vecChannelsIn[packet->m_nChannel] = malloc(offsetof(RTMPPacket,m_header));
  memcpy(r->m_vecChannelsIn[packet->m_nChannel], packet, offsetof(RTMPPacket,m_header));

  if (RTMPPacket_IsReady(packet))
  {
    packet->m_nTimeStamp = packet->m_nInfoField1;
    
    // make packet's timestamp absolute 
    if (!packet->m_hasAbsTimestamp) 
      packet->m_nTimeStamp += r->m_channelTimestamp[packet->m_nChannel]; // timestamps seem to be always relative!! 
      
    r->m_channelTimestamp[packet->m_nChannel] = packet->m_nTimeStamp; 
 
    // reset the data from the stored packet. we keep the header since we may use it later if a new packet for this channel
    // arrives and requests to re-use some info (small packet header)
    r->m_vecChannelsIn[packet->m_nChannel]->m_nBytesRead = 0;
    r->m_vecChannelsIn[packet->m_nChannel]->m_hasAbsTimestamp = false; // can only be false if we reuse header
  }

  return true;
}

static int EncodeString(char *output, const AVal *strName, const AVal *strValue)
{
  char *buf = output;
  buf += AMF_EncodeInt16(output, strName->av_len);

  memcpy(buf, strName->av_val, strName->av_len);
  buf += strName->av_len;
  
  buf += AMF_EncodeString(buf, strValue);
  return buf - output;
}

static int EncodeNumber(char *output, const AVal *strName, double dVal)
{
  char *buf = output;
  buf += AMF_EncodeInt16(output, strName->av_len);

  memcpy(buf, strName->av_val, strName->av_len);
  buf += strName->av_len;

  buf += AMF_EncodeNumber(buf, dVal);
  return buf - output;
}

static int EncodeBoolean(char *output, const AVal *strName, bool bVal)
{
  char *buf = output;
  buf += AMF_EncodeInt16(output, strName->av_len);

  memcpy(buf, strName->av_val, strName->av_len);
  buf += strName->av_len;

  buf += AMF_EncodeBoolean(buf, bVal);

  return buf - output;
}

#ifdef CRYPTO
#include "hand2.c"
#else
static bool HandShake(RTMP *r, bool FP9HandShake)
{
  int i;
  char clientsig[RTMP_SIG_SIZE+1];
  char serversig[RTMP_SIG_SIZE];

  clientsig[0] = 0x03; // not encrypted
  
  uint32_t uptime = htonl(RTMP_GetTime());
  memcpy(clientsig + 1, &uptime, 4);

  memset(&clientsig[5], 0, 4);

#ifdef _DEBUG
    for (i=9; i<RTMP_SIG_SIZE; i++) 
      clientsig[i] = 0xff;
#else
    for (i=9; i<RTMP_SIG_SIZE; i++)
      clientsig[i] = (char)(rand() % 256);
#endif

  if (!WriteN(r, clientsig, RTMP_SIG_SIZE + 1))
    return false;

  char type;
  if (ReadN(r, &type, 1) != 1) // 0x03 or 0x06
    return false;

  Log(LOGDEBUG, "%s: Type Answer   : %02X", __FUNCTION__, type);
  
  if(type != clientsig[0])
  	Log(LOGWARNING, "%s: Type mismatch: client sent %d, server answered %d", __FUNCTION__, clientsig[0], type);

  if (ReadN(r, serversig, RTMP_SIG_SIZE) != RTMP_SIG_SIZE)
    return false;

  // decode server response
  uint32_t suptime;

  memcpy(&suptime, serversig, 4);
  suptime = ntohl(suptime);

  Log(LOGDEBUG, "%s: Server Uptime : %d", __FUNCTION__, suptime);
  Log(LOGDEBUG, "%s: FMS Version   : %d.%d.%d.%d", __FUNCTION__, serversig[4], serversig[5], serversig[6], serversig[7]);

  // 2nd part of handshake
  if (!WriteN(r, serversig, RTMP_SIG_SIZE))
    return false;

  char resp[RTMP_SIG_SIZE];
  if (ReadN(r, resp, RTMP_SIG_SIZE) != RTMP_SIG_SIZE)
    return false;

  bool bMatch = (memcmp(resp, clientsig + 1, RTMP_SIG_SIZE) == 0);
  if (!bMatch)
  {
    Log(LOGWARNING, "%s, client signature does not match!",__FUNCTION__);
  }
  return true;
}
#endif

static bool SendRTMP(RTMP *r, RTMPPacket *packet, bool queue)
{
  const RTMPPacket *prevPacket = r->m_vecChannelsOut[packet->m_nChannel];
  if (prevPacket && packet->m_headerType != RTMP_PACKET_SIZE_LARGE)
  {
    // compress a bit by using the prev packet's attributes
    if (prevPacket->m_nBodySize == packet->m_nBodySize && packet->m_headerType == RTMP_PACKET_SIZE_MEDIUM) 
      packet->m_headerType = RTMP_PACKET_SIZE_SMALL;

    if (prevPacket->m_nInfoField2 == packet->m_nInfoField2 && packet->m_headerType == RTMP_PACKET_SIZE_SMALL)
      packet->m_headerType = RTMP_PACKET_SIZE_MINIMUM;
      
  }

  if (packet->m_headerType > 3) // sanity
  { 
    Log(LOGERROR, "sanity failed!! trying to send header of type: 0x%02x.", (unsigned char)packet->m_headerType);
    return false;
  }

  int nSize = packetSize[packet->m_headerType];
  int hSize = nSize;
  char *header = packet->m_body - nSize;
  header[0] = (char)((packet->m_headerType << 6) | packet->m_nChannel);
  if (nSize > 1)
    AMF_EncodeInt24(header+1, packet->m_nInfoField1);
  
  if (nSize > 4)
  {
    AMF_EncodeInt24(header+4, packet->m_nBodySize);
    header[7] = packet->m_packetType;
  }

  if (nSize > 8)
    EncodeInt32LE(header+8, packet->m_nInfoField2);

  nSize = packet->m_nBodySize;
  char *buffer = packet->m_body;
  int nChunkSize = RTMP_DEFAULT_CHUNKSIZE;

  while (nSize)
  {
    int wrote;

    if (nSize < nChunkSize)
      nChunkSize = nSize;

    if (header) {
      wrote=WriteN(r, header, nChunkSize+hSize);
      header = NULL;
    } else {
      wrote=WriteN(r, buffer, nChunkSize);
    }
    if (!wrote)
      return false;

    nSize -= nChunkSize;
    buffer += nChunkSize;

    if (nSize > 0)
    {
      header = buffer-1;
      hSize = 1;
      *header = (0xc0 | packet->m_nChannel);
    }
  }

  if (packet->m_packetType == 0x14 && queue) { // we invoked a remote method, keep it in call queue till result arrives
    AVal method;
    AMF_DecodeString(packet->m_body+1, &method);
    AV_queue(&r->m_methodCalls, &r->m_numCalls, &method);
    Log(LOGDEBUG, "Invoking %s", method.av_val);
  }

  if (!r->m_vecChannelsOut[packet->m_nChannel])
    r->m_vecChannelsOut[packet->m_nChannel] = malloc(offsetof(RTMPPacket,m_header));
  memcpy(r->m_vecChannelsOut[packet->m_nChannel], packet, offsetof(RTMPPacket,m_header));
  return true;
}

void RTMP_Close(RTMP *r)
{
  int i;

  if (RTMP_IsConnected(r))
    close(r->m_socket);

  r->m_stream_id = -1;
  r->m_socket = 0;
  r->m_chunkSize = RTMP_DEFAULT_CHUNKSIZE;
  r->m_nBWCheckCounter = 0;
  r->m_nBytesIn = 0;
  r->m_nBytesInSent = 0;
  r->m_nClientBW = 2500000;
  r->m_nClientBW2 = 2;
  r->m_nServerBW = 2500000;

  for (i=0; i<RTMP_CHANNELS; i++)
  {
    if (r->m_vecChannelsIn[i]) {
	  free(r->m_vecChannelsIn[i]);
	  r->m_vecChannelsIn[i] = NULL;
	}
	if (r->m_vecChannelsOut[i]) {
	  free(r->m_vecChannelsOut[i]);
	  r->m_vecChannelsOut[i] = NULL;
	}
  }
  AV_clear(r->m_methodCalls, r->m_numCalls);
  r->m_methodCalls = NULL;
  r->m_numCalls = 0;

  r->m_bPlaying = false;
  r->m_nBufferSize = 0;
}

static bool FillBuffer(RTMP *r)
{
    assert(r->m_nBufferSize == 0); // only fill buffer when it's empty
    int nBytes;

again:
    nBytes = recv(r->m_socket, r->m_pBuffer, sizeof(r->m_pBuffer), 0);
    if(nBytes != -1) {
    	r->m_nBufferSize += nBytes;
	r->m_pBufferStart = r->m_pBuffer;
    }
    else
    {
      int sockerr = GetSockError();
      Log(LOGDEBUG, "%s, recv returned %d. GetSockError(): %d (%s)", __FUNCTION__, nBytes,
         sockerr, strerror(sockerr));
      if (sockerr == EINTR && !bCtrlC)
        goto again;

      if (sockerr == EWOULDBLOCK || sockerr == EAGAIN)
        r->m_bTimedout = true;
      else
        RTMP_Close(r);
      return false;
    }

  return true;
}

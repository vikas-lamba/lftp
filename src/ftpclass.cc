/*
 * lftp and utils
 *
 * Copyright (c) 1996-2004 by Alexander V. Lukyanov (lav@yars.free.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id$ */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#include "ftpclass.h"
#include "xstring.h"
#include "xmalloc.h"
#include "url.h"
#include "FtpListInfo.h"
#include "FileGlob.h"
#include "FtpDirList.h"
#include "log.h"
#include "FileCopyFtp.h"
#include "LsCache.h"
#include "buffer_ssl.h"

#include "ascii_ctype.h"
#include "misc.h"

enum {FTP_TYPE_A,FTP_TYPE_I};

#define TELNET_IAC	255		/* interpret as command: */
#define	TELNET_IP	244		/* interrupt process--permanently */
#define	TELNET_DM	242		/* for telfunc calls */

#include <errno.h>
#include <time.h>

#ifdef TM_IN_SYS_TIME
# include <sys/time.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

CDECL_BEGIN
#include <regex.h>
CDECL_END

#if defined(HAVE_INET_ATON) && !defined(HAVE_INET_ATON_DECL)
CDECL int inet_aton(const char *,struct in_addr *);
#endif

#include "xalloca.h"

#ifdef USE_SSL
# include "lftp_ssl.h"
#else
# define control_ssl 0
const bool Ftp::ftps=false;
#endif

#define max_buf 0x10000

#define FTP_DEFAULT_PORT "21"
#define FTPS_DEFAULT_PORT "990"
#define FTP_DATA_PORT 20
#define FTPS_DATA_PORT 989
#define HTTP_DEFAULT_PROXY_PORT "3128"

#define super NetAccess

#define is5XX(code) ((code)>=500 && (code)<600)
#define is4XX(code) ((code)>=400 && (code)<500)
#define is3XX(code) ((code)>=300 && (code)<400)
#define is2XX(code) ((code)>=200 && (code)<300)
#define is1XX(code) ((code)>=100 && (code)<200)

#define debug(a) Log::global->Format a

FileAccess *Ftp::New() { return new Ftp(); }

void  Ftp::ClassInit()
{
   // register the class
   Register("ftp",Ftp::New);
   FileCopy::fxp_create=FileCopyFtp::New;

#ifdef USE_SSL
   Register("ftps",FtpS::New);
#endif
}


#if INET6

struct eprt_proto_match
{
   int proto;
   int eprt_proto;
};
static const eprt_proto_match eprt_proto[]=
{
   { AF_INET,  1 },
   { AF_INET6, 2 },
   { -1, -1 }
};

const char *Ftp::encode_eprt(sockaddr_u *a)
{
   char host[NI_MAXHOST];
   char serv[NI_MAXSERV];
   static char *eprt=0;

   int proto=-1;
   for(const eprt_proto_match *p=eprt_proto; p->proto!=-1; p++)
   {
      if(a->sa.sa_family==p->proto)
      {
	 proto=p->eprt_proto;
	 break;
      }
   }
   if(proto==-1)
      return 0;

   if(getnameinfo(&a->sa, sizeof(*a), host, sizeof(host), serv, sizeof(serv),
      NI_NUMERICHOST | NI_NUMERICSERV) < 0)
   {
      return 0;
   }
   eprt=(char*)xrealloc(eprt,20+strlen(host)+strlen(serv));
   sprintf(eprt, "|%d|%s|%s|", proto, host, serv);
   return eprt;
}
#endif

bool Ftp::Connection::data_address_ok(sockaddr_u *dp,bool verify_address,bool verify_port)
{
   sockaddr_u d;
   sockaddr_u c;
   socklen_t len;
   len=sizeof(d);
   if(dp)
      d=*dp;
   else if(getpeername(data_sock,&d.sa,&len)==-1)
   {
      Log::global->Format(0,"getpeername(data_sock): %s\n",strerror(errno));
      return false;
   }
   len=sizeof(c);
   if(getpeername(control_sock,&c.sa,&len)==-1)
   {
      Log::global->Format(0,"getpeername(control_sock): %s\n",strerror(errno));
      return false;
   }

#if INET6
   if(d.sa.sa_family==AF_INET && c.sa.sa_family==AF_INET6
      && IN6_IS_ADDR_V4MAPPED(&c.in6.sin6_addr))
   {
      if(memcmp(&d.in.sin_addr,&c.in6.sin6_addr.s6_addr[12],4))
	 goto address_mismatch;
      if(d.in.sin_port!=htons(FTP_DATA_PORT)
      && d.in.sin_port!=htons(FTPS_DATA_PORT))
	 goto wrong_port;
   }
#endif

   if(d.sa.sa_family!=c.sa.sa_family)
      return false;
   if(d.sa.sa_family==AF_INET)
   {
      if(memcmp(&d.in.sin_addr,&c.in.sin_addr,sizeof(d.in.sin_addr)))
	 goto address_mismatch;
      if(d.in.sin_port!=htons(FTP_DATA_PORT)
      && d.in.sin_port!=htons(FTPS_DATA_PORT))
	 goto wrong_port;
      return true;
   }
#if INET6
# ifndef  IN6_ARE_ADDR_EQUAL
#  define IN6_ARE_ADDR_EQUAL(a,b) (!memcmp((a),(b),16))
# endif
   if(d.sa.sa_family==AF_INET6)
   {
      if(!IN6_ARE_ADDR_EQUAL(&d.in6.sin6_addr,&c.in6.sin6_addr))
	 goto address_mismatch;
      if(d.in6.sin6_port!=htons(FTP_DATA_PORT)
      && d.in6.sin6_port!=htons(FTPS_DATA_PORT))
         goto wrong_port;
      return true;
   }
#endif
   return true;

wrong_port:
   if(!verify_port)
      return true;
   Log::global->Format(0,"**** %s\n",_("Data connection peer has wrong port number"));
   return false;

address_mismatch:
   if(!verify_address)
      return true;
   Log::global->Format(0,"**** %s\n",_("Data connection peer has mismatching address"));
   return false;
}

/* Procedures for checking for a special answers */
/* General rule: check for valid replies first, errors second. */

void Ftp::RestCheck(int act)
{
   if(is2XX(act) || is3XX(act))
   {
      real_pos=rest_pos;  // REST successful
      last_rest=rest_pos;
      return;
   }
   real_pos=0;
   if(pos==0)
      return;
   if(is5XX(act))
   {
      if(act==RESP_NOT_IMPLEMENTED || act==RESP_NOT_UNDERSTOOD)
	 rest_supported=false;
      DebugPrint("---- ",_("Switching to NOREST mode"),2);
      flags|=NOREST_MODE;
      if(mode==STORE)
	 pos=0;
      if(copy_mode!=COPY_NONE)
	 copy_failed=true;
      return;
   }
   Disconnect();
}

void Ftp::NoFileCheck(int act)
{
   if(is2XX(act))
      return;
   if(act==RESP_NOT_IMPLEMENTED || act==RESP_NOT_UNDERSTOOD)
   {
      SetError(FATAL,all_lines);
      return;
   }
   if(is5XX(act) && !Transient5XX(act))
   {
      if(real_pos>0 && !(flags&IO_FLAG) && copy_mode==COPY_NONE)
      {
	 DataClose();
	 DebugPrint("---- ",_("Switching to NOREST mode"),2);
	 flags|=NOREST_MODE;
	 real_pos=0;
	 if(mode==STORE)
	    pos=0;
	 state=EOF_STATE; // retry
	 return;
      }
      SetError(NO_FILE,all_lines);
      return;
   }
   DataClose();
   state=EOF_STATE;
   retry_time=now+2; // retry after 2 seconds
}

/* 5xx that aren't errors at all */
bool Ftp::NonError5XX(int act)
{
   return (mode==LIST && act==RESP_NO_FILE && (!file || !file[0]))
       // ...and proftpd workaround.
       || (mode==LIST && act==450 && strstr(line,"No files found"));
}

/* 5xx that are really transient like 4xx */
bool Ftp::Transient5XX(int act)
{
   if(!is5XX(act))
      return false;

   // retry on these errors (ftp server ought to send 4xx code instead)
   if((strstr(line,"Broken pipe") && (!file || !strstr(file,"Broken pipe")))
   || (strstr(line,"Too many")    && (!file || !strstr(file,"Too many")))
   || (strstr(line,"timed out")   && (!file || !strstr(file,"timed out")))
   // if there were some data received, assume it is temporary error.
   || (mode!=STORE && (flags&IO_FLAG)))
      return true;

   return false;
}

// 226 Transfer complete.
void Ftp::TransferCheck(int act)
{
   if(conn->data_sock==-1)
      eof=true;
   if(act==225 || act==226) // data connection is still open or ABOR worked.
   {
      copy_done=true;
      AbortedClose();
   }
   if(act==211)
   {
      // permature STAT?
      stat_time=now+3;
      return;
   }
   if(act==213)	  // this must be a STAT reply.
   {
      stat_time=now;

      long long p;
      // first try Serv-U format:
      //    Status for user UUU from X.X.X.X
      //    Stored 1 files, 0 Kbytes
      //    Retrieved 0 files, 0 Kbytes
      //    Receiving file XXX (YYY bytes)
      char *r=strstr(all_lines,"Receiving file");
      if(r)
      {
	 r=strrchr(r,'(');
	 char c=0;
	 if(r && sscanf(r,"(%lld bytes%c",&p,&c)==2 && c==')')
	    goto found_offset;
      }
      // wu-ftpd format:
      //    Status: XXX of YYY bytes transferred
      // or
      //    Status: XXX bytes transferred
      //
      // find the first number.
      for(char *b=line+4; ; b++)
      {
	 if(*b==0)
	    return;
	 if(!is_ascii_digit(*b))
	    continue;
	 if(sscanf(b,"%lld",&p)==1)
	    break;
      }
   found_offset:
      if(copy_mode==COPY_DEST)
	 real_pos=pos=p;
      return;
   }
   if(copy_mode!=COPY_NONE && act==425)
   {
      copy_passive=!copy_passive;
      copy_failed=true;
      return;
   }
   if(NonError5XX(act))
   {
      DataClose();
      state=EOF_STATE;
      eof=true; // simulate eof
      return;
   }
   if(act==RESP_BROKEN_PIPE && copy_mode==COPY_NONE)
   {
      if(conn->data_sock==-1 && strstr(line,"Broken pipe"))
   	 return;
   }
   NoFileCheck(act);
}

void Ftp::LoginCheck(int act)
{
   if(ignore_pass)
      return;
   if(act==530) // login incorrect or overloaded server
   {
      const char *rexp=Query("retry-530",hostname);
      if(re_match(all_lines,rexp,REG_ICASE))
      {
	 DebugPrint("---- ",_("Server reply matched ftp:retry-530, retrying"));
	 goto retry;
      }
      if(!user)
      {
	 rexp=Query("retry-530-anonymous",hostname);
	 if(re_match(all_lines,rexp,REG_ICASE))
	 {
	    DebugPrint("---- ",_("Server reply matched ftp:retry-530-anonymous, retrying"));
	    goto retry;
	 }
      }
   }
   if(is5XX(act))
   {
      SetError(LOGIN_FAILED,all_lines);
      return;
   }

   if(!is2XX(act) && !is3XX(act))
   {
   retry:
      Disconnect();
      NextPeer();
      if(peer_curr==0)
	 try_time=now;	// count the reconnect-interval from this moment
   }
   if(is3XX(act))
   {
      if(!QueryStringWithUserAtHost("acct"))
      {
	 Disconnect();
	 SetError(LOGIN_FAILED,_("Account is required, set ftp:acct variable"));
      }
   }
}

void Ftp::NoPassReqCheck(int act) // for USER command
{
   if(is2XX(act)) // in some cases, ftpd does not ask for pass.
   {
      ignore_pass=true;
      return;
   }
   if(act==331 && allow_skey && user && pass)
   {
      skey_pass=xstrdup(make_skey_reply());
      if(force_skey && skey_pass==0)
      {
	 SetError(LOGIN_FAILED,_("ftp:skey-force is set and server does not support OPIE nor S/KEY"));
	 return;
      }
   }
   if(is3XX(act))
      return;
   if(act==530)	  // no such user or overloaded server
   {
      // Unfortunately, at this point we cannot tell if it is
      // really incorrect login or overloaded server, because
      // many overloaded servers return hard error 530...
      // So try to check the message for `user unknown'.
      // NOTE: modern ftp servers don't say `user unknown', they wait for
      // PASS and then say `Login incorrect'.
      if(strstr(line,"unknown")) // Don't translate!!!
      {
	 DebugPrint("---- ",_("Saw `unknown', assume failed login"));
	 SetError(LOGIN_FAILED,all_lines);
	 return;
      }
      goto def_ret;
   }
   if(is5XX(act))
   {
      // proxy interprets USER as user@host, so we check for host name validity
      if(proxy && (strstr(line,"host") || strstr(line,"resolve")))
      {
	 DebugPrint("---- ",_("assuming failed host name lookup"));
	 SetError(LOOKUP_ERROR,all_lines);
	 return;
      }
      SetError(LOGIN_FAILED,all_lines);
      return;
   }
def_ret:
   Disconnect();
   try_time=now;	// count the reconnect-interval from this moment
}

// login to proxy.
void Ftp::proxy_LoginCheck(int act)
{
   if(is2XX(act))
      return;
   if(is5XX(act))
   {
      SetError(LOGIN_FAILED,all_lines);
      return;
   }
   Disconnect();
   try_time=now;	// count the reconnect-interval from this moment
}

void Ftp::proxy_NoPassReqCheck(int act)
{
   if(is3XX(act) || is2XX(act))
      return;
   if(is5XX(act))
   {
      SetError(LOGIN_FAILED,all_lines);
      return;
   }
   Disconnect();
   try_time=now;	// count the reconnect-interval from this moment
}

static void normalize_path_vms(char *path)
{
   for(char *s=path; *s; s++)
      *s=to_ascii_lower(*s);
   char *colon=strchr(path,':');
   if(colon)
   {
      memmove(path+1,path,strlen(path)+1);
      path[0]='/';
      path=colon+1;
      if(path[1]=='[')
	 memmove(path,path+1,strlen(path));
   }
   else
   {
      path=strchr(path,'[');
      if(!*path)
	 return;
   }
   *path++='/';
   while(*path && *path!=']')
   {
      if(*path=='.')
	 *path='/';
      path++;
   }
   if(!*path)
      return;
   if(path[1])
      *path='/';
   else
      *path=0;
}

char *Ftp::ExtractPWD()
{
   // drop \0 according to RFC2640
   for(int i=0; i<line_len; i++)
   {
      if(line[i]==0)
      {
	 memmove(line+i,line+i+1,line_len-i);
	 line_len--;
      }
   }

   char *pwd=string_alloca(strlen(line)+1);

   const char *scan=strchr(line,'"');
   if(scan==0)
      return 0;
   scan++;
   const char *right_quote=strrchr(scan,'"');
   if(!right_quote)
      return 0;

   char *store=pwd;
   while(scan<right_quote)
   {
      // This is the method of quote encoding.
      if(*scan=='"' && scan[1]=='"')
	 scan++;
      *store++=*scan++;
   }

   if(store==pwd)
      return 0;	  // empty home not allowed.
   *store=0;

   int dev_len=device_prefix_len(pwd);
   if(pwd[dev_len]=='[')
   {
      vms_path=true;
      normalize_path_vms(pwd);
   }
   else if(dev_len==2 || dev_len==3)
   {
      dos_path=true;
   }

   if(!strchr(pwd,'/') || dos_path)
   {
      // for safety -- against dosish ftpd
      for(char *s=pwd; *s; s++)
	 if(*s=='\\')
	    *s='/';
   }
   return xstrdup(pwd);
}

void Ftp::SendCWD(const char *path,check_case_t c)
{
   SendCmd2("CWD",path);
   AddResp(RESP_CWD_RMD_DELE_OK,c);
   SetRespPath(path);
}

static bool InPrivateNetwork(const sockaddr_u *u)
{
   if(u->sa.sa_family==AF_INET)
   {
      unsigned char *a=(unsigned char *)&u->in.sin_addr;
      return (a[0]==10)
	  || (a[0]==172 && a[1]>=16 && a[1]<32)
	  || (a[0]==192 && a[1]==168);
   }
   return false;
}
static bool IsLoopback(const sockaddr_u *u)
{
   if(u->sa.sa_family==AF_INET)
   {
      unsigned char *a=(unsigned char *)&u->in.sin_addr;
      return (a[0]==127 && a[1]==0 && a[2]==0 && a[3]==1);
   }
#if INET6
   if(u->sa.sa_family==AF_INET6)
      return IN6_IS_ADDR_LOOPBACK(&u->in6.sin6_addr);
#endif
   return false;
}

Ftp::pasv_state_t Ftp::Handle_PASV()
{
   unsigned a0,a1,a2,a3,p0,p1;
   /*
    * Extract address. RFC1123 says:
    * "...must scan the reply for the first digit..."
    */
   for(char *b=line+4; ; b++)
   {
      if(*b==0)
      {
	 Disconnect();
	 return PASV_NO_ADDRESS_YET;
      }
      if(!is_ascii_digit(*b))
	 continue;
      if(sscanf(b,"%u,%u,%u,%u,%u,%u",&a0,&a1,&a2,&a3,&p0,&p1)==6)
         break;
   }
   unsigned char *a,*p;
   data_sa.sa.sa_family=conn->peer_sa.sa.sa_family;
   if(data_sa.sa.sa_family==AF_INET)
   {
      a=(unsigned char*)&data_sa.in.sin_addr;
      p=(unsigned char*)&data_sa.in.sin_port;
   }
#if INET6
   else if(data_sa.sa.sa_family==AF_INET6)
   {
      a=((unsigned char*)&data_sa.in6.sin6_addr)+12;
      a[-1]=a[-2]=0xff; // V4MAPPED
      p=(unsigned char*)&data_sa.in6.sin6_port;
   }
#endif
   else
   {
      Disconnect();
      return PASV_NO_ADDRESS_YET;
   }

   a[0]=a0; a[1]=a1; a[2]=a2; a[3]=a3;
   p[0]=p0; p[1]=p1;

   if((a0==0 && a1==0 && a2==0 && a3==0)
   || (QueryBool("fix-pasv-address",hostname) && !proxy_is_http
       && (InPrivateNetwork(&data_sa) != InPrivateNetwork(&conn->peer_sa))))
   {
      // broken server, try to fix up
      conn->fixed_pasv=true;
      DebugPrint("---- ","Address returned by PASV seemed to be incorrect and has been fixed",2);
      if(data_sa.sa.sa_family==AF_INET)
	 memcpy(a,&conn->peer_sa.in.sin_addr,sizeof(conn->peer_sa.in.sin_addr));
#if INET6
      else if(data_sa.in.sin_family==AF_INET6)	// peer_sa should be V4MAPPED
	 memcpy(a,&conn->peer_sa.in6.sin6_addr.s6_addr[12],4);
#endif
   }

   return PASV_HAVE_ADDRESS;
}

Ftp::pasv_state_t Ftp::Handle_EPSV()
{
   char delim;
   char *format=alloca_strdup("|||%u|");
   unsigned port;
   char *c;

   c=strchr(line,'(');
   c=c?c+1:line+4;
   delim=*c;

   for(char *p=format; *p; p++)
      if(*p=='|')
	 *p=delim;

   if(sscanf(c,format,&port)!=1)
   {
      DebugPrint("**** ",_("cannot parse EPSV response"),0);
      Disconnect();
      return PASV_NO_ADDRESS_YET;
   }

   socklen_t len=sizeof(data_sa);
   getpeername(conn->control_sock,&data_sa.sa,&len);
   if(data_sa.sa.sa_family==AF_INET)
      data_sa.in.sin_port=htons(port);
#if INET6
   else if(data_sa.sa.sa_family==AF_INET6)
      data_sa.in6.sin6_port=htons(port);
#endif
   else
   {
      Disconnect();
      return PASV_NO_ADDRESS_YET;
   }
   return PASV_HAVE_ADDRESS;
}

void Ftp::CatchDATE(int act)
{
   if(!array_for_info)
      return;

   if(is2XX(act))
   {
      if(strlen(line)>4 && is_ascii_digit(line[4]))
	 array_for_info[array_ptr].time=ConvertFtpDate(line+4);
      else
	 array_for_info[array_ptr].time=NO_DATE;
   }
   else	if(is5XX(act))
   {
      if(act==500 || act==502)
	 mdtm_supported=false;
      array_for_info[array_ptr].time=NO_DATE;
   }
   else
   {
      Disconnect();
      return;
   }

   array_for_info[array_ptr].get_time=false;
   if(!array_for_info[array_ptr].get_size)
      array_ptr++;

   retries=0;
   persist_retries=0;
}
void Ftp::CatchDATE_opt(int act)
{
   if(!opt_date)
      return;

   if(is2XX(act) && strlen(line)>4 && is_ascii_digit(line[4]))
   {
      *opt_date=ConvertFtpDate(line+4);
      opt_date=0;
   }
   else
   {
      if(act==500 || act==502)
	 mdtm_supported=false;
      *opt_date=NO_DATE;
   }
}

void Ftp::CatchSIZE(int act)
{
   if(!array_for_info)
      return;

   long long size=NO_SIZE;

   if(is2XX(act))
   {
      if(strlen(line)>4 && is_ascii_digit(line[4]))
	 sscanf(line+4,"%lld",&size);
   }
   else	if(is5XX(act))
   {
      if(act==500 || act==502)
	 size_supported=false;
   }
   else
   {
      Disconnect();
      return;
   }

   if(size<1)
      size=NO_SIZE;

   array_for_info[array_ptr].size=size;

   array_for_info[array_ptr].get_size=false;
   if(!array_for_info[array_ptr].get_time)
      array_ptr++;

   retries=0;
   persist_retries=0;
}
void Ftp::CatchSIZE_opt(int act)
{
   long long size=NO_SIZE;

   if(is2XX(act) && strlen(line)>4 && is_ascii_digit(line[4]))
   {
      sscanf(line+4,"%lld",&size);
   }
   else
   {
      if(act==500 || act==502)
	 size_supported=false;
   }

   if(size<1)
      return;

   entity_size=size;

   if(opt_size)
   {
      *opt_size=entity_size;
      opt_size=0;
   }
}

Ftp::Connection::Connection()
{
   control_sock=-1;
   control_recv=control_send=0;
   telnet_layer_send=0;
   send_cmd_buffer=new Buffer;
   data_sock=-1;
   data_iobuf=0;
   aborted_data_sock=-1;
#ifdef USE_SSL
   control_ssl=0;
   data_ssl=0;
   prot='C';  // current protection scheme 'C'lear or 'P'rivate
   auth_sent=false;
#endif

   quit_sent=false;
   fixed_pasv=false;
   translation_activated=false;
   sync_wait=1;	// expect server greetings
   multiline_code=0;
}

void Ftp::InitFtp()
{
   conn=0;

#ifdef USE_SSL
   ftps=false;	  // ssl and prot='P' by default (port 990)
   auth_supported=true;
   auth_args_supported=0;
#endif

   nop_time=0;
   nop_count=0;
   nop_offset=0;
   line=0;
   line_len=0;
   all_lines=0;
   eof=false;
   type=FTP_TYPE_A;
   state=INITIAL_STATE;
   flags=SYNC_MODE;
   wait_flush=false;
   ignore_pass=false;
   try_feat_after_login=false;
   skey_pass=0;
   allow_skey=true;
   force_skey=false;
   verify_data_address=true;
   list_options=0;
   use_stat=true;
   stat_interval=1;

   use_mdtm=true;
   use_size=true;
   use_telnet_iac=true;
   use_pret=true;

   dos_path=false;
   vms_path=false;
   mdtm_supported=true;
   size_supported=true;
   rest_supported=true;
   site_chmod_supported=true;
   site_utime_supported=true;
   pret_supported=false;
   utf8_supported=false;
   lang_supported=false;
   last_rest=0;
   rest_pos=0;

   RespQueue=0;
   RQ_alloc=0;
   RQ_head=RQ_tail=0;

   anon_pass=0;
   anon_user=0;	  // will be set by Reconfig()

   charset=0;

   copy_mode=COPY_NONE;
   copy_addr_valid=false;
   copy_passive=false;
   copy_done=false;
   copy_connection_open=false;
   stat_time=0;
   copy_allow_store=false;
   copy_failed=false;

   memset(&data_sa,0,sizeof(data_sa));

   disconnect_on_close=false;

   Reconfig();

   retry_time=0;
}
Ftp::Ftp() : super()
{
   InitFtp();
}
Ftp::Ftp(const Ftp *f) : super(f)
{
   InitFtp();

   state=INITIAL_STATE;
   flags=f->flags&MODES_MASK;
}

Ftp::Connection::~Connection()
{
   CloseDataConnection();
   if(control_sock!=-1)
   {
      Log::global->Format(7,"---- %s\n",_("Closing control socket"));
      close(control_sock);
   }
   Delete(control_send);
   Delete(control_recv);
   delete send_cmd_buffer;
}

Ftp::~Ftp()
{
   Enter();
   Close();
   Disconnect();
   if(conn)
   {
      FlushSendQueue();
      ReceiveResp();
   }
   Disconnect();

   xfree(anon_user);
   xfree(anon_pass);
   xfree(charset);
   xfree(list_options);
   xfree(line);
   xfree(all_lines);

   xfree(RespQueue);
   xfree(skey_pass);

#ifdef USE_SSL
   xfree(auth_args_supported);
#endif
   Leave();
}

bool Ftp::AbsolutePath(const char *s)
{
   if(!s)
      return false;
   int dev_len=device_prefix_len(s);
   return(s[0]=='/'
      || (((dos_path && dev_len==3) || (vms_path && dev_len>2))
	  && s[dev_len-1]=='/'));
}

// returns true if we need to sleep instead of moving to higher level.
bool Ftp::GetBetterConnection(int level,bool limit_reached)
{
   bool need_sleep=false;

   if(level==0 && cwd==0)
      return need_sleep;

   for(FA *fo=FirstSameSite(); fo!=0; fo=NextSameSite(fo))
   {
      Ftp *o=(Ftp*)fo; // we are sure it is Ftp.

      if(o->GetConnectLevel()!=CL_LOGGED_IN)
	 continue;
      if(!SameConnection(o))
	 continue;

      if(level==0 && xstrcmp(real_cwd,o->real_cwd))
	 continue;

      if(o->conn->data_sock!=-1 || o->state!=EOF_STATE || o->mode!=CLOSED)
      {
	 /* session is in use; last resort is to takeover an active connection */
	 if(level<2)
	    continue;
	 /* only take over lower priority or suspended jobs */
	 if(!connection_takeover || (o->priority>=priority && !o->suspended))
	    continue;
	 if(o->conn->data_sock!=-1 && o->RespQueueSize()<=1)
	 {
	    /* don't take over active connections if they won't be able to resume */
	    if((o->flags&NOREST_MODE) && o->real_pos>0x1000)
	       continue;
	    if(o->QueryBool("web-mode",o->hostname))
	       continue;
	    o->DataAbort();
	    o->DataClose();
	    if(!o->conn)
	       return need_sleep; // oops...
	 }
	 else
	 {
	    if(o->RespQueueSize()>0 || o->disconnect_on_close)
	       continue;
	 }
      }
      else
      {
	 if(limit_reached)
	 {
	    /* wait until job is diff seconds idle before taking it over */
	    int diff=o->last_priority-priority;
	    if(diff>0)
	    {
	       /* number of seconds the task has been idle */
	       int have_idle=now-o->idle_start;
	       if(have_idle<diff)
	       {
		  TimeoutS(diff-have_idle);
		  need_sleep=true;
		  continue;
	       }
	    }
	 }
      }

      // so borrow the connection
      MoveConnectionHere(o);
      return false;
   }
   return need_sleep;
}

void  Ftp::HandleTimeout()
{
   conn->quit_sent=true;
   super::HandleTimeout();
}

void Ftp::Connection::SavePeerAddress()
{
   socklen_t addr_len=sizeof(peer_sa);
   getpeername(control_sock,&peer_sa.sa,&addr_len);
}
// Create buffers after control socket had been connected.
void Ftp::Connection::MakeBuffers()
{
   control_send=new IOBufferFDStream(
      new FDStream(control_sock,"control-socket"),IOBuffer::PUT);
   control_recv=new IOBufferFDStream(
      new FDStream(control_sock,"control-socket"),IOBuffer::GET);
}
void Ftp::Connection::InitTelnetLayer()
{
   if(telnet_layer_send)
      return;
   control_send=telnet_layer_send=new IOBufferTelnet(control_send);
   control_recv=new IOBufferTelnet(control_recv);
}

int   Ftp::Do()
{
   char	 *str=(char*)alloca(xstrlen(cwd)+xstrlen(hostname)+xstrlen(proxy)+256);
   const char *command=0;
   bool	       append_file=false;
   int   res;
   socklen_t addr_len;
   unsigned char *a;
   unsigned char *p;
   automate_state oldstate;
   int	 m=STALL;

   // check if idle time exceeded
   if(mode==CLOSED && conn && idle>0)
   {
      if(now >= idle_start+idle)
      {
	 DebugPrint("---- ",_("Closing idle connection"),1);
	 Disconnect();
	 if(conn)
	    idle_start=now+timeout;
	 return m;
      }
      TimeoutS(idle_start+idle-time_t(now));
   }

   if(conn && conn->quit_sent)
   {
      m|=FlushSendQueue();
      m|=ReceiveResp();
      if(RespQueueIsEmpty())
      {
	 Disconnect();
	 return MOVED;
      }
      goto usual_return;
   }

   if(Error() || eof || mode==CLOSED)
   {
      // inactive behavior
      if(conn)
      {
	 m|=FlushSendQueue();
	 m|=ReceiveResp();
      }
      if(eof || mode==CLOSED)
	 goto notimeout_return;
      goto usual_return;
   }

   if(!hostname)
      return m;

   switch(state)
   {
   case(INITIAL_STATE):
   {
      // walk through ftp classes and try to find identical idle ftp session
      // first try "easy" cases of session take-over.
      for(int i=0; i<3; i++)
      {
	 bool limit_reached=(connection_limit>0
			    && connection_limit<=CountConnections());
	 if(i>=2 && !limit_reached)
	    break;
	 bool need_sleep=GetBetterConnection(i,limit_reached);
	 if(state!=INITIAL_STATE)
	    return MOVED;
	 if(need_sleep)
	    return m;
      }

      if(!ReconnectAllowed())
	 return m;

      proxy_is_http=ProxyIsHttp();

      if(ftps)
	 m|=Resolve(FTPS_DEFAULT_PORT,"ftps","tcp");
      else
	 m|=Resolve(FTP_DEFAULT_PORT,"ftp","tcp");
      if(!peer)
	 return m;

      if(mode==CONNECT_VERIFY)
	 return m;

      if(!NextTry())
	 return MOVED;

      assert(!conn);
      conn=new Connection();

      conn->peer_sa=peer[peer_curr];
      conn->control_sock=SocketCreateTCP(conn->peer_sa.sa.sa_family);
      if(conn->control_sock==-1)
      {
	 delete conn;
	 conn=0;
	 if(peer_curr+1<peer_num)
	 {
	    try_time=0;
	    peer_curr++;
	    retries--;
	    return MOVED;
	 }
	 sprintf(str,"socket: %s",strerror(errno));
         DebugPrint("**** ",str,0);
	 if(NonFatalError(errno))
	    return m;
	 sprintf(str,_("cannot create socket of address family %d"),
			conn->peer_sa.sa.sa_family);
	 SetError(SEE_ERRNO,str);
	 return MOVED;
      }
      KeepAlive(conn->control_sock);
      SetSocketBuffer(conn->control_sock);
      SetSocketMaxseg(conn->control_sock);
      NonBlock(conn->control_sock);
      CloseOnExec(conn->control_sock);

      SayConnectingTo();

      res=SocketConnect(conn->control_sock,&conn->peer_sa);
      state=CONNECTING_STATE;
      if(res==-1
#ifdef EINPROGRESS
      && errno!=EINPROGRESS
#endif
      )
      {
	 int e=errno;
	 Log::global->Format(0,"connect(control_sock): %s\n",strerror(e));
	 if(NotSerious(e))
	 {
	    Disconnect();
	    return MOVED;
	 }
	 goto system_error;
      }
      m=MOVED;
      event_time=now;

      // reset *_supported on reconnect - we can possibly connect
      // to a different server in fact.
      size_supported=true;
      mdtm_supported=true;
      rest_supported=true;
      site_chmod_supported=true;
      site_utime_supported=true;
      last_rest=0;
   }

   case(CONNECTING_STATE):
      assert(conn && conn->control_sock!=-1);
      res=Poll(conn->control_sock,POLLOUT);
      if(res==-1)
      {
	 Disconnect();
	 return MOVED;
      }
      if(!(res&POLLOUT))
	 goto usual_return;

      conn->SavePeerAddress();

#ifdef USE_SSL
      if(proxy?!strcmp(proxy_proto,"ftps")||!strcmp(proxy_proto,"https"):ftps)
      {
	 conn->MakeSSLBuffers(hostname);
	 conn->prot='P';
      }
      else // note the following block
#endif
      {
	 conn->MakeBuffers();
      }

      if(!proxy || !proxy_is_http)
	 goto pre_CONNECTED_STATE;

      state=HTTP_PROXY_CONNECTED;
      m=MOVED;
      HttpProxySendConnect();

   case HTTP_PROXY_CONNECTED:
      if(!HttpProxyReplyCheck(conn->control_recv))
	 goto usual_return;

   pre_CONNECTED_STATE:
      if(use_telnet_iac)
	 conn->InitTelnetLayer();

      state=CONNECTED_STATE;
      m=MOVED;
      AddResp(RESP_READY,CHECK_READY);

      try_feat_after_login=false;
      if(use_feat)
      {
	 SendCmd("FEAT");
	 AddResp(211,CHECK_FEAT);
      }

      /* fallthrough */
   case CONNECTED_STATE:
   {
      m|=FlushSendQueue();
      m|=ReceiveResp();
      if(state!=CONNECTED_STATE || Error())
	 return MOVED;

      if(RespQueueHas(CHECK_FEAT))
	 return m;

#ifdef USE_SSL
      if(auth_supported && !conn->auth_sent && !conn->control_ssl && !ftps && (!proxy || proxy_is_http)
      && QueryBool((user && pass)?"ssl-allow":"ssl-allow-anonymous",hostname))
      {
	 const char *auth=Query("ssl-auth",hostname);
	 if(auth_args_supported)
	 {
	    char *a=alloca_strdup(auth_args_supported);
	    bool saw_ssl=false;
	    bool saw_tls=false;
	    for(a=strtok(a,";"); a; a=strtok(0,";"))
	    {
	       if(!strcasecmp(a,auth))
		  break;
	       if(!strcasecmp(a,"SSL"))
		  saw_ssl=true;
	       else if(!strcasecmp(a,"TLS"))
		  saw_tls=true;
	    }
	    if(!a)
	    {
	       const char *old_auth=auth;
	       if(saw_tls)
		  auth="TLS";
	       else if(saw_ssl)
		  auth="SSL";
	       Log::global->Format(1,
		  "**** AUTH %s is not supported, using AUTH %s instead\n",
		  old_auth,auth);
	    }
	 }
	 SendCmd2("AUTH",auth);
	 AddResp(234,CHECK_AUTH_TLS);
	 if(!strcmp(auth,"TLS")
	 || !strcmp(auth,"TLS-C"))
	    conn->prot='C';
	 else
	    conn->prot='P';
      }
      if(conn->auth_sent && !RespQueueIsEmpty())
	 goto usual_return;
#endif

      char *user_to_use=(user?user:anon_user);
      if(proxy && !proxy_is_http)
      {
	 char *combined=(char*)alloca(strlen(user_to_use)+1+strlen(hostname)+1+xstrlen(portname)+1);
	 sprintf(combined,"%s@%s",user_to_use,hostname);
	 if(portname)
	    sprintf(combined+strlen(combined),":%s",portname);
	 user_to_use=combined;

      	 if(proxy_user && proxy_pass)
	 {
	    AddResp(RESP_PASS_REQ,CHECK_USER_PROXY);
	    SendCmd2("USER",proxy_user);
	    AddResp(RESP_LOGGED_IN,CHECK_PASS_PROXY);
	    SendCmd2("PASS",proxy_pass);
	 }
      }

      xfree(skey_pass);
      skey_pass=0;

      ignore_pass=false;
      AddResp(RESP_PASS_REQ,CHECK_USER);
      SendCmd2("USER",user_to_use);

      state=USER_RESP_WAITING_STATE;
      m=MOVED;
   }

   case(USER_RESP_WAITING_STATE):
      if(((flags&SYNC_MODE) || (user && pass && allow_skey))
      && !RespQueueIsEmpty())
      {
	 m|=FlushSendQueue();
	 m|=ReceiveResp();
	 if(state!=USER_RESP_WAITING_STATE || Error())
	    return MOVED;
	 if(!RespQueueIsEmpty())
	    goto usual_return;
      }

      if(!ignore_pass)
      {
	 const char *pass_to_use=pass?pass:anon_pass;
	 if(allow_skey && skey_pass)
	    pass_to_use=skey_pass;
	 AddResp(RESP_LOGGED_IN,CHECK_PASS);
	 SendCmd2("PASS",pass_to_use);
      }

      if(try_feat_after_login)
      {
	 SendCmd("FEAT");
	 AddResp(211,CHECK_FEAT);
      }
      SendAcct();
      SendSiteGroup();
      SendSiteIdle();

      if(!home_auto)
      {
	 // if we don't yet know the home location, try to get it
	 SendCmd("PWD");
	 AddResp(RESP_PWD_MKD_OK,CHECK_PWD);
      }

#ifdef USE_SSL
      if(conn->control_ssl)
      {
	 SendCmd("PBSZ 0");
	 AddResp(0,CHECK_IGNORE);
      }
#endif // USE_SSL

      set_real_cwd(0);
      type=FTP_TYPE_A;	   // just after login we are in TEXT mode
      state=EOF_STATE;
      m=MOVED;

   case(EOF_STATE):
      m|=FlushSendQueue();
      m|=ReceiveResp();
      if(state!=EOF_STATE || Error())
	 return MOVED;

      if(RespQueueHas(CHECK_FEAT))
	 return m;

      if(!utf8_supported && charset && *charset)
	 conn->SetControlConnectionTranslation(charset);

      if(mode==CONNECT_VERIFY)
	 goto notimeout_return;

      if(mode==CHANGE_MODE && !site_chmod_supported)
      {
	 SetError(NO_FILE,_("SITE CHMOD is not supported by this site"));
	 return MOVED;
      }
      if(mode==CHANGE_MODE && !QueryBool("use-site-chmod"))
      {
	 SetError(NO_FILE,_("SITE CHMOD is disabled by ftp:use-site-chmod"));
	 return MOVED;
      }

      if(home==0 && !RespQueueIsEmpty())
	 goto usual_return;

      if(now<retry_time)
	 goto usual_return;
      retry_time=0;

      if(real_cwd==0)
	 set_real_cwd(home_auto);

      ExpandTildeInCWD();

      if(mode!=CHANGE_DIR)
      {
	 expected_response *last_cwd=FindLastCWD();
	 if((!last_cwd && xstrcmp(cwd,real_cwd) && !(real_cwd==0 && !xstrcmp(cwd,"~")))
	    || (last_cwd && xstrcmp(last_cwd->path,cwd)))
	 {
	    SendCWD(cwd,CHECK_CWD_CURR);
	 }
	 else if(last_cwd && !xstrcmp(last_cwd->path,cwd))
	 {
	    // no need for extra CWD, one's already sent.
	    last_cwd->check_case=CHECK_CWD_CURR;
	 }
      }
#ifdef USE_SSL
      if(conn->control_ssl)
      {
	 const char *want_prot="P";
	 if(mode==LIST || mode==LONG_LIST)
	    want_prot=QueryBool("ssl-protect-list",hostname)?"P":"C";
	 else
	    want_prot=QueryBool("ssl-protect-data",hostname)?"P":"C";
	 if(copy_mode!=COPY_NONE)
	    want_prot="C";
	 if(*want_prot!=conn->prot)
	 {
	    SendCmd2("PROT",want_prot);
	    AddResp(200,CHECK_PROT);
	 }
      }
#endif
      state=CWD_CWD_WAITING_STATE;
      m=MOVED;

   case CWD_CWD_WAITING_STATE:
   {
      m|=FlushSendQueue();
      m|=ReceiveResp();
      if(state!=CWD_CWD_WAITING_STATE || Error())
	 return MOVED;

      // wait for all CWD to finish
      if(mode!=CHANGE_DIR && FindLastCWD())
	 goto usual_return;
#ifdef USE_SSL
      // PROT is critical for data transfers
      if(RespQueueHas(CHECK_PROT))
	 goto usual_return;
#endif

      // address of peer is not known yet
      if(copy_mode!=COPY_NONE && !copy_passive && !copy_addr_valid)
	 goto usual_return;

      if(entity_size!=NO_SIZE && entity_size!=NO_SIZE_YET && entity_size<=pos
      && mode==RETRIEVE)
      {
	 eof=true;
	 goto pre_WAITING_STATE; // simulate eof.
      }

      if(!rest_supported)
	 flags|=NOREST_MODE;

      if(mode==STORE && (flags&NOREST_MODE) && pos>0)
	 pos=0;

      if(copy_mode==COPY_NONE
      && (mode==RETRIEVE || mode==STORE || mode==LIST || mode==LONG_LIST))
      {
	 assert(conn->data_sock==-1);
	 conn->data_sock=SocketCreateTCP(conn->peer_sa.sa.sa_family);
	 if(conn->data_sock==-1)
	 {
	    sprintf(str,"socket(data): %s",strerror(errno));
	    DebugPrint("**** ",str,0);
	    goto system_error;
	 }
	 NonBlock(conn->data_sock);
	 CloseOnExec(conn->data_sock);
	 KeepAlive(conn->data_sock);
	 SetSocketBuffer(conn->data_sock);
	 SetSocketMaxseg(conn->data_sock);

	 if(flags&PASSIVE_MODE)
	 {
	    if(QueryBool("bind-data-socket")
	    && !IsLoopback(&conn->peer_sa))
	    {
	       // connect should come from the same address, else server can refuse.
	       addr_len=sizeof(data_sa);
	       getsockname(conn->control_sock,&data_sa.sa,&addr_len);
	       if(data_sa.sa.sa_family==AF_INET)
		  data_sa.in.sin_port=0;
      #if INET6
	       else if(data_sa.sa.sa_family==AF_INET6)
		  data_sa.in6.sin6_port=0;
      #endif
	       if(bind(conn->data_sock,&data_sa.sa,addr_len)<0)
	       {
		  sprintf(str,"bind(data_sock): %s",strerror(errno));
		  DebugPrint("**** ",str,10);
	       }
	    }
	 }
	 else // !PASSIVE_MODE
	 {
	    addr_len=sizeof(data_sa);
	    if(copy_mode!=COPY_NONE)
	       data_sa=copy_addr;
	    else
	    {
	       getsockname(conn->control_sock,&data_sa.sa,&addr_len);

	       Range range(Query("port-range"));

	       for(int t=0; ; t++)
	       {
		  if(t>=10)
		  {
		     close(conn->data_sock);
		     conn->data_sock=-1;
		     TimeoutS(10);	 // retry later.
		     return m;
		  }
		  if(t==9)
		     ReuseAddress(conn->data_sock);   // try to reuse address.

		  int port=0;
		  if(!range.IsFull())
		     port=range.Random();

		  if(data_sa.sa.sa_family==AF_INET)
		     data_sa.in.sin_port=htons(port);
   #if INET6
		  else if(data_sa.sa.sa_family==AF_INET6)
		     data_sa.in6.sin6_port=htons(port);
   #endif
		  else
		  {
		     Fatal("unsupported network protocol");
		     return MOVED;
		  }

		  if(bind(conn->data_sock,&data_sa.sa,addr_len)==0)
		     break;

		  // Fail unless socket was already taken
		  if(errno!=EINVAL && errno!=EADDRINUSE)
		  {
		     sprintf(str,"bind: %s",strerror(errno));
		     DebugPrint("**** ",str,0);
		     goto system_error;
		  }
	       }
	       // get the allocated port
	       getsockname(conn->data_sock,&data_sa.sa,&addr_len);
	       listen(conn->data_sock,1);
	    }
	 }
      }

      int old_type=type;
      if((flags&NOREST_MODE) || pos==0)
	 real_pos=0;
      else
	 real_pos=-1;	// we don't yet know if REST will succeed

      flags&=~IO_FLAG;
      last_priority=priority;

      switch((enum open_mode)mode)
      {
      case(RETRIEVE):
	 if(file[0]==0)
	    goto long_list;
         type=FTP_TYPE_I;
	 command="RETR";
	 append_file=true;
         break;
      case(STORE):
         type=FTP_TYPE_I;
	 if(!QueryBool("rest-stor",hostname))
	 {
	    real_pos=0;	// some old servers don't handle REST/STOR properly.
	    pos=0;
	 }
         command="STOR";
	 append_file=true;
         break;
      long_list:
      case(LONG_LIST):
         type=FTP_TYPE_A;
         if(!rest_list)
	    real_pos=0;	// some ftp servers do not do REST/LIST.
	 command="LIST";
	 if(list_options && list_options[0])
	 {
	    char *c=string_alloca(5+strlen(list_options)+1);
	    sprintf(c,"LIST %s",list_options);
	    command=c;
	 }
	 if(file && file[0])
	    append_file=true;
         break;
      case(LIST):
         type=FTP_TYPE_A;
         real_pos=0; // REST doesn't work for NLST
	 command="NLST";
	 if(file && file[0])
            append_file=true;
         break;
      case(CHANGE_DIR):
	 if(!xstrcmp(real_cwd,file))
	 {
	    xfree(cwd);
	    cwd=xstrdup(real_cwd);
	 }
	 else
	 {
	    int len=xstrlen(real_cwd);
	    char *path_to_use=file;
	    if(!vms_path && !AbsolutePath(file) && real_cwd
	    && !strncmp(file,real_cwd,len) && file[len]=='/')
	       path_to_use=file+len+1;
	    SendCWD(path_to_use,CHECK_CWD);
	    SetRespPath(file);
	 }
	 goto pre_WAITING_STATE;
      case(MAKE_DIR):
	 command="MKD";
	 append_file=true;
	 break;
      case(REMOVE_DIR):
	 command="RMD";
	 append_file=true;
	 break;
      case(REMOVE):
	 command="DELE";
	 append_file=true;
	 break;
      case(QUOTE_CMD):
	 real_pos=0;
	 command="";
	 append_file=true;
	 assert(!conn->data_iobuf);
	 assert(!rate_limit);
	 conn->data_iobuf=new IOBuffer(IOBuffer::GET);
	 rate_limit=new RateLimit(hostname);
	 break;
      case(RENAME):
	 command="RNFR";
	 append_file=true;
	 break;
      case(ARRAY_INFO):
	 type=FTP_TYPE_I;
	 break;
      case(CHANGE_MODE):
	 {
	    char *c=string_alloca(11+30);
	    sprintf(c,"SITE CHMOD %03o",chmod_mode);
	    command=c;
	    append_file=true;
	    break;
	 }
      case(CONNECT_VERIFY):
      case(CLOSED):
	 abort(); // can't happen
      }
      if(ascii)
	 type=FTP_TYPE_A;
      if(old_type!=type)
      {
	 SendCmd2("TYPE",type==FTP_TYPE_I?"I":"A");
	 AddResp(RESP_TYPE_OK);
      }

      if(opt_size && size_supported && file[0] && use_size)
      {
	 SendCmd2("SIZE",file);
	 AddResp(RESP_RESULT_HERE,CHECK_SIZE_OPT);
      }
      if(opt_date && mdtm_supported && file[0] && use_mdtm)
      {
	 SendCmd2("MDTM",file);
	 AddResp(RESP_RESULT_HERE,CHECK_MDTM_OPT);
      }

      if(mode==ARRAY_INFO)
      {
	 SendArrayInfoRequests();
	 goto pre_WAITING_STATE;
      }

      if(mode==QUOTE_CMD || mode==CHANGE_MODE
      || mode==REMOVE || mode==REMOVE_DIR || mode==MAKE_DIR || mode==RENAME)
      {
	 if(mode==MAKE_DIR && mkdir_p)
	 {
	    char *sl=strchr(file,'/');
	    while(sl)
	    {
	       if(sl>file)
	       {
		  *sl=0;
		  if(strcmp(file,".") && strcmp(file,".."))
		  {
		     SendCmd2("MKD",file);
		     AddResp(0,CHECK_IGNORE);
		  }
		  *sl='/';
	       }
	       sl=strchr(sl+1,'/');
	    }
	 }

	 if(append_file)
	    SendCmd2(command,file);
	 else
	    SendCmd(command);

	 if(mode==REMOVE_DIR || mode==REMOVE)
	    AddResp(RESP_CWD_RMD_DELE_OK,CHECK_FILE_ACCESS);
	 else if(mode==MAKE_DIR)
	    AddResp(RESP_PWD_MKD_OK,CHECK_FILE_ACCESS);
	 else if(mode==QUOTE_CMD)
	 {
	    AddResp(0,CHECK_QUOTED);
	    if(!strncasecmp(file,"CWD",3)
	    || !strncasecmp(file,"CDUP",4)
	    || !strncasecmp(file,"XCWD",4)
	    || !strncasecmp(file,"XCUP",4))
	    {
	       DebugPrint("---- ","Resetting cwd",9);
	       set_real_cwd(0);  // we do not know the path now.
	    }
	 }
	 else if(mode==RENAME)
	    AddResp(350,CHECK_RNFR);
	 else
	    AddResp(200,CHECK_FILE_ACCESS);

	 goto pre_WAITING_STATE;
      }

      if((copy_mode==COPY_NONE && (flags&PASSIVE_MODE))
      || (copy_mode!=COPY_NONE && copy_passive))
      {
	 if(use_pret && pret_supported)
	 {
	    char *s=string_alloca(5+strlen(command)+1+strlen(file)+2);
	    strcpy(s, "PRET ");
	    strcat(s, command);
	    strcat(s, " ");
	    strcat(s, file);
	    SendCmd(s);
	    AddResp(200,CHECK_PRET);
	 }
	 if(conn->peer_sa.sa.sa_family==AF_INET)
	 {
#if INET6
	 ipv4_pasv:
#endif
	    SendCmd("PASV");
	    AddResp(227,CHECK_PASV);
	    pasv_state=PASV_NO_ADDRESS_YET;
	 }
	 else
	 {
#if INET6
	    if(conn->peer_sa.sa.sa_family==AF_INET6
	    && IN6_IS_ADDR_V4MAPPED(&conn->peer_sa.in6.sin6_addr))
	       goto ipv4_pasv;
	    SendCmd("EPSV");
	    AddResp(229,CHECK_EPSV);
	    pasv_state=PASV_NO_ADDRESS_YET;
#else
	    Fatal(_("unsupported network protocol"));
	    return MOVED;
#endif
	 }
      }
      else // !PASSIVE
      {
	 if(copy_mode!=COPY_NONE)
	    data_sa=copy_addr;
	 if(data_sa.sa.sa_family==AF_INET)
	 {
	    a=(unsigned char*)&data_sa.in.sin_addr;
	    p=(unsigned char*)&data_sa.in.sin_port;
#if INET6
	 ipv4_port:
#endif
	    const char *port_ipv4=Query("port-ipv4",hostname);
	    struct in_addr fake_ip;
	    if(port_ipv4 && port_ipv4[0])
	    {
	       if(inet_aton(port_ipv4,&fake_ip))
		  a=(unsigned char*)&fake_ip;
	    }
	    sprintf(str,"PORT %d,%d,%d,%d,%d,%d",a[0],a[1],a[2],a[3],p[0],p[1]);
	    SendCmd(str);
	    AddResp(RESP_PORT_OK,CHECK_PORT);
	 }
	 else
	 {
#if INET6
	    if(data_sa.sa.sa_family==AF_INET6
	       && IN6_IS_ADDR_V4MAPPED(&data_sa.in6.sin6_addr))
	    {
	       a=((unsigned char*)&data_sa.in6.sin6_addr)+12;
	       p=(unsigned char*)&data_sa.in6.sin6_port;
	       goto ipv4_port;
	    }
	    SendCmd2("EPRT",encode_eprt(&data_sa));
	    AddResp(RESP_PORT_OK,CHECK_PORT);
#else
	    Fatal(_("unsupported network protocol"));
	    return MOVED;
#endif
	 }
      }
      // some broken servers don't reset REST after a transfer,
      // so check if last_rest was different.
      if(real_pos==-1 || last_rest!=real_pos)
      {
         rest_pos=(real_pos!=-1?real_pos:pos);
	 sprintf(str,"REST %lld",(long long)rest_pos);
	 real_pos=-1;
	 SendCmd(str);
	 AddResp(RESP_REST_OK,CHECK_REST);
      }
      if(copy_mode!=COPY_DEST || copy_allow_store)
      {
	 if(append_file)
	    SendCmd2(command,file);
	 else
	    SendCmd(command);
	 AddResp(RESP_TRANSFER_OK,CHECK_TRANSFER);
      }
      m=MOVED;
      if(copy_mode!=COPY_NONE && !copy_passive)
	 goto pre_WAITING_STATE;
      if((copy_mode==COPY_NONE && (flags&PASSIVE_MODE))
      || (copy_mode!=COPY_NONE && copy_passive))
      {
	 state=DATASOCKET_CONNECTING_STATE;
      	 goto datasocket_connecting_state;
      }
      state=ACCEPTING_STATE;
   }
   case(ACCEPTING_STATE):
      m|=FlushSendQueue();
      m|=ReceiveResp();

      if(state!=ACCEPTING_STATE || Error())
         return MOVED;

      res=Poll(conn->data_sock,POLLIN);
      if(res==-1)
      {
	 Disconnect();
         return MOVED;
      }

      if(!(res&POLLIN))
	 goto usual_return;

      addr_len=sizeof(struct sockaddr);
      res=accept(conn->data_sock,(struct sockaddr *)&data_sa,&addr_len);
      if(res==-1)
      {
	 if(errno==EWOULDBLOCK)
	    goto usual_return;
	 if(NotSerious(errno))
	 {
	    DebugPrint("**** ",strerror(errno),0);
	    Disconnect();
	    return MOVED;
	 }
	 goto system_error;
      }

      close(conn->data_sock);
      conn->data_sock=res;
      NonBlock(conn->data_sock);
      CloseOnExec(conn->data_sock);
      KeepAlive(conn->data_sock);
      SetSocketBuffer(conn->data_sock);
      SetSocketMaxseg(conn->data_sock);

      if(!conn->data_address_ok(0,verify_data_address,verify_data_port))
      {
	 Disconnect();
	 return MOVED;
      }

      goto pre_data_open;

   case(DATASOCKET_CONNECTING_STATE):
   datasocket_connecting_state:
      m|=FlushSendQueue();
      m|=ReceiveResp();

      if(state!=DATASOCKET_CONNECTING_STATE || Error())
         return MOVED;

      switch(pasv_state)
      {
      case PASV_NO_ADDRESS_YET:
	 goto usual_return;

      case PASV_HAVE_ADDRESS:
	 if(copy_mode==COPY_NONE
	 && !conn->data_address_ok(&data_sa,verify_data_address,/*port_verify*/false))
	 {
	    Disconnect();
	    return MOVED;
	 }

	 pasv_state=PASV_DATASOCKET_CONNECTING;
	 if(copy_mode!=COPY_NONE)
	 {
	    memcpy(&copy_addr,&data_sa,sizeof(data_sa));
	    copy_addr_valid=true;
	    goto pre_WAITING_STATE;
	 }

	 if(!proxy_is_http)
	 {
	    sprintf(str,_("Connecting data socket to (%s) port %u"),
	       SocketNumericAddress(&data_sa),SocketPort(&data_sa));
	    DebugPrint("---- ",str,5);
	    res=SocketConnect(conn->data_sock,&data_sa);
	 }
	 else // proxy_is_http
	 {
	    sprintf(str,_("Connecting data socket to proxy %s (%s) port %u"),
	       proxy,SocketNumericAddress(&conn->peer_sa),SocketPort(&conn->peer_sa));
	    DebugPrint("---- ",str,5);
	    res=SocketConnect(conn->data_sock,&conn->peer_sa);
	 }
	 if(res==-1
#ifdef EINPROGRESS
	 && errno!=EINPROGRESS
#endif
	 )
	 {
	    sprintf(str,"connect: %s",strerror(errno));
	    DebugPrint("**** ",str,0);
	    Disconnect();
	    if(NotSerious(errno))
	       return MOVED;
	    goto system_error;
	 }
	 m=MOVED;
      case PASV_DATASOCKET_CONNECTING:
	 res=Poll(conn->data_sock,POLLOUT);
	 if(res==-1)
	 {
	    if(conn->fixed_pasv)
	    {
	       DebugPrint("---- ",_("Switching passive mode off"),2);
	       SetFlag(PASSIVE_MODE,0);
	    }
	    Disconnect();
	    return MOVED;
	 }
	 if(!(res&POLLOUT))
	    goto usual_return;
	 if(!proxy_is_http)
	    goto pre_data_open;

	 pasv_state=PASV_HTTP_PROXY_CONNECTED;
	 m=MOVED;
	 conn->data_iobuf=new IOBufferFDStream(new FDStream(conn->data_sock,"data-socket"),IOBuffer::PUT);
	 HttpProxySendConnectData();
	 Roll(conn->data_iobuf);
	 Delete(conn->data_iobuf); // FIXME, it could be not done yet
	 conn->data_iobuf=new IOBufferFDStream(new FDStream(conn->data_sock,"data-socket"),IOBuffer::GET);
      case PASV_HTTP_PROXY_CONNECTED:
      	 if(HttpProxyReplyCheck(conn->data_iobuf))
	    goto pre_data_open;
	 return m;
      }

   pre_data_open:
      assert(rate_limit==0);
      rate_limit=new RateLimit(hostname);
      state=DATA_OPEN_STATE;
      m=MOVED;

      Delete(conn->data_iobuf);
      conn->data_iobuf=0;
#ifdef USE_SSL
      if(conn->prot=='P')
      {
	 conn->data_ssl=lftp_ssl_new(conn->data_sock,hostname);
	 // share session id between control and data connections.
	 SSL_copy_session_id(conn->data_ssl,conn->control_ssl);

	 IOBuffer::dir_t dir=(mode==STORE?IOBuffer::PUT:IOBuffer::GET);
	 IOBufferSSL *ssl_buf=new IOBufferSSL(conn->data_ssl,dir,hostname);
	 ssl_buf->DoConnect();
	 ssl_buf->CloseLater();
	 conn->data_iobuf=ssl_buf;
      }
      else  // note the following block
#endif
      {
	 IOBuffer::dir_t dir=(mode==STORE?IOBuffer::PUT:IOBuffer::GET);
	 conn->data_iobuf=new IOBufferFDStream(new FDStream(conn->data_sock,"data-socket"),dir);
      }
      if(mode==LIST || mode==LONG_LIST)
      {
	 if(utf8_supported)
	    conn->data_iobuf->SetTranslation("UTF-8",true);
	 else if(charset && *charset)
	    conn->data_iobuf->SetTranslation(charset,true);
      }

   case(DATA_OPEN_STATE):
   {
      if(RespQueueIsEmpty() && conn->data_sock!=-1)
      {
	 // When ftp server has sent "Transfer complete" it is idle,
	 // but the data can be still unsent in server side kernel buffer.
	 // So the ftp server can decide the connection is idle for too long
	 // time and disconnect. This hack is to prevent the above.
	 if(now >= nop_time+nop_interval)
	 {
	    // prevent infinite NOOP's
	    if(nop_offset==pos && nop_count*nop_interval>=timeout)
	    {
	       HandleTimeout();
	       return MOVED;
	    }
	    if(nop_time!=0)
	    {
	       nop_count++;
	       SendCmd("NOOP");
	       AddResp(0,CHECK_IGNORE);
	    }
	    nop_time=now;
	    if(nop_offset!=pos)
	       nop_count=0;
	    nop_offset=pos;
	 }
	 TimeoutS(nop_interval-(time_t(now)-nop_time));
      }

      oldstate=state;

      m|=FlushSendQueue();
      m|=ReceiveResp();

      if(state!=oldstate || Error())
         return MOVED;

      BumpEventTime(conn->data_iobuf->EventTime());
      if(conn->data_iobuf->Error())
      {
	 DebugPrint("**** ",conn->data_iobuf->ErrorText(),0);
	 if(conn->data_iobuf->ErrorFatal())
	    SetError(FATAL,conn->data_iobuf->ErrorText());
	 conn->quit_sent=true;
	 Disconnect();
	 return MOVED;
      }
      if(mode!=STORE)
      {
	 if(conn->data_iobuf->Size()>=rate_limit->BytesAllowedToGet())
	 {
	    conn->data_iobuf->Suspend();
	    Timeout(1000);
	 }
	 else if(conn->data_iobuf->Size()>=max_buf)
	 {
	    conn->data_iobuf->Suspend();
	    m=MOVED;
	 }
	 else if(conn->data_iobuf->IsSuspended())
	 {
	    conn->data_iobuf->Resume();
	    if(conn->data_iobuf->Size()>0)
	       m=MOVED;
	 }
	 if(conn->data_iobuf->Size()==0 && conn->data_iobuf->Eof())
	 {
	    DataClose();
	    if(RespQueueIsEmpty())
	    {
	       eof=true;
	       m=MOVED;
	    }
	    state=WAITING_STATE;
	 }
      }

      if(state!=oldstate || Error())
         return MOVED;

      CheckTimeout();

      if(state!=oldstate)
         return MOVED;

      goto usual_return;
   }

   pre_WAITING_STATE:
      if(copy_mode!=COPY_NONE)
      {
	 retries=0;  // it is enough to get here in copying.
	 persist_retries=0;
      }
      state=WAITING_STATE;
      m=MOVED;
   case(WAITING_STATE):
   {
      oldstate=state;

      m|=FlushSendQueue();
      m|=ReceiveResp();

      if(state!=oldstate || Error())
         return MOVED;

      // more work to do?
      if(RespQueueIsEmpty() && mode==ARRAY_INFO && array_ptr<array_cnt)
      {
	 SendArrayInfoRequests();
	 return MOVED;
      }

      if(RespQueueIsEmpty() && conn->data_sock==-1 && conn->data_iobuf && !conn->data_iobuf->Eof())
      {
	 conn->data_iobuf->PutEOF();
	 m=MOVED;
      }
      if(conn->data_iobuf && conn->data_iobuf->Eof() && conn->data_iobuf->Size()==0)
      {
	 state=EOF_STATE;
	 DataAbort();
	 DataClose();
	 set_idle_start();
	 eof=true;
	 return MOVED;
      }

      if(copy_mode==COPY_DEST && !copy_allow_store)
	 goto notimeout_return;

      if(copy_mode==COPY_DEST && !copy_done && copy_connection_open
      && RespQueueSize()==1 && use_stat && !conn->control_ssl && !proxy_is_http)
      {
	 if(stat_time+stat_interval<=now)
	 {
	    // send STAT to know current position.
	    SendUrgentCmd("STAT");
	    AddResp(213,CHECK_TRANSFER);
	    FlushSendQueue(true);
	    m=MOVED;
	 }
	 else
	    TimeoutS(stat_time+stat_interval-now);
      }

      // FXP is special - no data connection at all.
      if(copy_mode!=COPY_NONE)
	 goto notimeout_return;

      goto usual_return;
   }
   } /* end of switch */
   return m;

usual_return:
   if(m==MOVED)
      return MOVED;
   if(CheckTimeout())
      return MOVED;
notimeout_return:
   if(m==MOVED)
      return MOVED;
   if(conn && conn->data_sock!=-1)
   {
      if(state==ACCEPTING_STATE)
	 Block(conn->data_sock,POLLIN);
      else if(state==DATASOCKET_CONNECTING_STATE)
      {
	 if(pasv_state==PASV_DATASOCKET_CONNECTING)
	    Block(conn->data_sock,POLLOUT);
      }
   }
   if(conn && conn->control_sock!=-1)
   {
      if(state==CONNECTING_STATE)
	 Block(conn->control_sock,POLLOUT);
   }
   return m;

system_error:
   if(NonFatalError(errno))
      return m;
   conn->quit_sent=true;
   Disconnect();
   SetError(SEE_ERRNO,0);
   return MOVED;
}

void Ftp::SendSiteIdle()
{
   if(!QueryBool("use-site-idle"))
      return;
   SendCmd2("SITE IDLE",idle);
   AddResp(0,CHECK_IGNORE);
}
void Ftp::SendUTimeRequest()
{
   if(entity_date==NO_DATE || !file)
      return;
   if(QueryBool("use-site-utime") && site_utime_supported)
   {
      char *c=string_alloca(11+strlen(file)+14*3+3+4);
      char d[15];
      time_t n=now;
      strftime(d,sizeof(d)-1,"%Y%m%d%H%M%S",gmtime(&n));
      sprintf(c,"SITE UTIME %s %s %s %s UTC",file,d,d,d);
      SendCmd(c);
      AddResp(200,CHECK_SITE_UTIME);
   }
   else if(QueryBool("use-mdtm-overloaded"))
   {
      char *c=string_alloca(5+14+1);
      time_t n=now;
      strftime(c,19,"MDTM %Y%m%d%H%M%S",gmtime(&n));
      SendCmd2(c,file);
      AddResp(0,CHECK_IGNORE);
   }
}
const char *Ftp::QueryStringWithUserAtHost(const char *var)
{
   const char *u=user?user:"anonymous";
   const char *h=hostname?hostname:"";
   char *closure=string_alloca(strlen(u)+1+strlen(h)+1);
   sprintf(closure,"%s@%s",u,h);
   const char *val=Query(var,closure);
   if(!val || !val[0])
      val=Query(var,hostname);
   if(!val || !val[0])
      return 0;
   return val;
}
void Ftp::SendAcct()
{
   const char *acct=QueryStringWithUserAtHost("acct");
   if(!acct)
      return;
   SendCmd2("ACCT",acct);
   AddResp(0,CHECK_IGNORE);
}
void Ftp::SendSiteGroup()
{
   const char *group=QueryStringWithUserAtHost("site-group");
   if(!group)
      return;
   SendCmd2("SITE GROUP",group);
   AddResp(0,CHECK_IGNORE);
}

void Ftp::SendArrayInfoRequests()
{
   for(int i=array_ptr; i<array_cnt; i++)
   {
      bool sent=false;
      if(array_for_info[i].get_time && mdtm_supported && use_mdtm)
      {
	 SendCmd2("MDTM",ExpandTildeStatic(array_for_info[i].file));
	 AddResp(RESP_RESULT_HERE,CHECK_MDTM);
	 sent=true;
      }
      else
      {
	 array_for_info[i].time=NO_DATE;
      }
      if(array_for_info[i].get_size && size_supported && use_size)
      {
	 SendCmd2("SIZE",ExpandTildeStatic(array_for_info[i].file));
	 AddResp(RESP_RESULT_HERE,CHECK_SIZE);
	 sent=true;
      }
      else
      {
	 array_for_info[i].size=NO_SIZE;
      }
      if(!sent)
      {
	 if(i==array_ptr)
	    array_ptr++;   // if it is first one, just skip it.
	 else
	    break;	   // otherwise, wait until it is first.
      }
      else
      {
	 if(flags&SYNC_MODE)
	    break;	   // don't flood the queues.
      }
   }
}

int Ftp::ReplyLogPriority(int code)
{
   // Greeting messages
   if(code==220 || code==230)
      return 3;
   if(code==250 && mode==CHANGE_DIR)
      return 3;
   if(code==451 && mode==CLOSED)
      return 4;
   /* Most 5XXs go to level 4, as it's the job's responsibility to
    * print fatal errors. Some 5XXs are treated as 4XX's; send those
    * to level 0. (Maybe they should go to 1; we're going to retry them,
    * after all. */
   if(is5XX(code))
      return Transient5XX(code)? 0:4;

   if(is4XX(code))
      return 0;

   // 221 is the reply to QUIT, but we don't expect it.
   if(code==221 && RespQueue[RQ_head].expect!=221)
      return 0;

   return 4;
}

#ifdef USE_SSL
void Ftp::BlockOnSSL(SSL *ssl)
{
   int fd=SSL_get_fd(ssl);
   if(SSL_want_read(ssl))
      current->Block(fd,POLLIN);
   if(SSL_want_write(ssl))
      current->Block(fd,POLLOUT);
}
#endif

int  Ftp::ReceiveResp()
{
   int m=STALL;

   if(!conn || !conn->control_recv)
      return m;

   BumpEventTime(conn->control_recv->EventTime());
   if(conn->control_recv->Error())
   {
      DebugPrint("**** ",conn->control_recv->ErrorText(),0);
      if(conn->control_recv->ErrorFatal())
	 SetError(FATAL,conn->control_recv->ErrorText());
      conn->quit_sent=true;
      Disconnect();
      return MOVED;
   }

   for(;;)  // handle all lines in buffer, one line per loop
   {
      if(!conn || !conn->control_recv)
	 return m;

      const char *resp;
      int resp_size;
      conn->control_recv->Get(&resp,&resp_size);
      if(resp==0) // eof
      {
	 DebugPrint("**** ",_("Peer closed connection"),0);
	 conn->quit_sent=true;
	 Disconnect();
	 return MOVED;
      }
      const char *nl=(const char*)memchr(resp,'\n',resp_size);
      if(!nl)
      {
	 if(conn->control_recv->Eof())
	    nl=resp+resp_size;
	 else
	    return m;
      }
      m=MOVED;

      xfree(line);
      line_len=nl-resp;
      line=(char*)xmalloc(line_len+1);
      memcpy(line,resp,line_len);
      line[line_len]=0;
      conn->control_recv->Skip(line_len+1);
      if(line_len>0 && line[line_len-1]=='\r')
	 line[--line_len]=0;
      for(char *scan=line+line_len-1; scan>=line; scan--)
      {
	 if(*scan=='\0')
	    *scan='!';
      }

      int code=0;

      if(strlen(line)>=3 && is_ascii_digit(line[0])
      && is_ascii_digit(line[1]) && is_ascii_digit(line[2]))
	 code=atoi(line);

      DebugPrint("<--- ",line,
	    ReplyLogPriority(conn->multiline_code?conn->multiline_code:code));
      if(!RespQueueIsEmpty() && RespQueue[RQ_head].check_case==CHECK_QUOTED && conn->data_iobuf)
      {
	 conn->data_iobuf->Put(line);
	 conn->data_iobuf->Put("\n");
      }

      int all_lines_len=xstrlen(all_lines);
      if(conn->multiline_code==0 || all_lines_len==0)
	 all_lines_len=-1; // not continuation
      all_lines=(char*)xrealloc(all_lines,all_lines_len+1+strlen(line)+1);
      if(all_lines_len>0)
	 all_lines[all_lines_len]='\n';
      strcpy(all_lines+all_lines_len+1,line);

      if(code==0)
	 continue;

      if(line[3]=='-')
      {
	 if(conn->multiline_code==0)
	    conn->multiline_code=code;
	 continue;
      }
      if(conn->multiline_code)
      {
	 if(conn->multiline_code!=code || line[3]!=' ')
	    continue;   // Multiline response can terminate only with
			// the same code as it started with.
			// The space is required.
	 conn->multiline_code=0;
      }
      if(conn->sync_wait>0 && code/100!=1)
	 conn->sync_wait--; // clear the flag to send next command

      CheckResp(code);
      m=MOVED;
      if(error_code==NO_FILE || error_code==LOGIN_FAILED)
      {
	 if(error_code==LOGIN_FAILED)
	    try_time=now;	// count the reconnect-interval from this moment
	 if(persist_retries++<max_persist_retries)
	 {
	    error_code=OK;
	    Disconnect();
	    DebugPrint("---- ",_("Persist and retry"),4);
	    return m;
	 }
      }
   }
   return m;
}

void Ftp::HttpProxySendConnect()
{
   const char *the_port=portname?portname:ftps?FTPS_DEFAULT_PORT:FTP_DEFAULT_PORT;
   conn->control_send->Format("CONNECT %s:%s HTTP/1.0\r\n\r\n",hostname,the_port);
   Log::global->Format(4,"+--> CONNECT %s:%s HTTP/1.0\n",hostname,the_port);
   http_proxy_status_code=0;
}
void Ftp::HttpProxySendConnectData()
{
   const char *the_host=SocketNumericAddress(&data_sa);
   int the_port=SocketPort(&data_sa);
   conn->data_iobuf->Format("CONNECT %s:%d HTTP/1.0\r\n\r\n",the_host,the_port);
   Log::global->Format(4,"+--> CONNECT %s:%d HTTP/1.0\n",the_host,the_port);
   http_proxy_status_code=0;
}
// Check reply and return true when the reply is received and is ok.
bool Ftp::HttpProxyReplyCheck(IOBuffer *buf)
{
   const char *b;
   int s;
   buf->Get(&b,&s);
   const char *nl=b?(const char*)memchr(b,'\n',s):0;
   if(!nl)
   {
      if(buf->Error())
      {
	 DebugPrint("**** ",buf->ErrorText(),0);
	 if(buf->ErrorFatal())
	    SetError(FATAL,buf->ErrorText());
      }
      else if(buf->Eof())
	 DebugPrint("**** ",_("Peer closed connection"),0);
      if(buf->Eof() || buf->Error())
      {
	 conn->quit_sent=true;
	 Disconnect();
      }
      return false;
   }

   char *line=string_alloca(nl-b);
   memcpy(line,b,nl-b-1);	 // don't copy \r
   line[nl-b-1]=0;
   buf->Skip(nl-b+1);	 // skip \r\n too.

   DebugPrint("<--+ ",line,4);

   if(!http_proxy_status_code)
   {
      if(1!=sscanf(line,"HTTP/%*d.%*d %d",&http_proxy_status_code)
      || !is2XX(http_proxy_status_code))
      {
	 // check for retriable codes
	 if(http_proxy_status_code==408 // Request Timeout
	 || http_proxy_status_code==502 // Bad Gateway
	 || http_proxy_status_code==503 // Service Unavailable
	 || http_proxy_status_code==504)// Gateway Timeout
	 {
	    conn->quit_sent=true;
	    Disconnect();
	    return false;
	 }
	 SetError(FATAL,all_lines);
	 conn->quit_sent=true;
	 Disconnect();
	 return false;
      }
   }
   if(!*line)
      return true;
   return false;
}

void Ftp::SendUrgentCmd(const char *cmd)
{
   if(!use_telnet_iac || !conn->telnet_layer_send)
   {
      SendCmd(cmd);
      return;
   }

   static const char pre_cmd[]={TELNET_IAC,TELNET_IP,TELNET_IAC,TELNET_DM};

#ifdef USE_SSL
   if(conn->control_ssl)
   {
      // no way to send urgent data over ssl, send normally.
      conn->telnet_layer_send->Buffer::Put(pre_cmd,4);
   }
   else // note the following block
#endif
   {
      int fl=fcntl(conn->control_sock,F_GETFL);
      fcntl(conn->control_sock,F_SETFL,fl&~O_NONBLOCK);
      FlushSendQueue(/*all=*/true);
      if(!conn || !conn->control_send)
	 return;
      if(conn->control_send->Size()>0)
	 Roll(conn->control_send);
      /* send only first byte as OOB due to OOB braindamage in many unices */
      send(conn->control_sock,pre_cmd,1,MSG_OOB);
      send(conn->control_sock,pre_cmd+1,3,0);
      fcntl(conn->control_sock,F_SETFL,fl);
   }
   SendCmd(cmd);
}

void  Ftp::DataAbort()
{
   if(!conn || state==CONNECTING_STATE || conn->quit_sent)
      return;

   if(conn->data_sock==-1 && copy_mode==COPY_NONE)
      return; // nothing to abort

   if(copy_mode!=COPY_NONE)
   {
      if(RespQueueIsEmpty())
	 return; // the transfer seems to be finished
      if(!copy_addr_valid)
	 return; // data connection cannot be established at this time
      if(!copy_connection_open && RespQueueSize()==1)
      {
	 // wu-ftpd-2.6.0 cannot interrupt accept() or connect().
	 conn->quit_sent=true;
	 Disconnect();
	 return;
      }
   }
   copy_connection_open=false;

   // if transfer has been completed then ABOR is not needed
   if(conn->data_sock!=-1 && RespQueueIsEmpty())
      return;

   CloseRespQueue();

   if(!conn)	// CloseRespQueue can disconnect.
      return;

   if(!QueryBool("use-abor",hostname)
   || RespQueueSize()>1 || proxy_is_http)
   {
      // check that we have a data socket to close, and the server is not
      // in uninterruptible accept() state.
      if(copy_mode==COPY_NONE
      && !((flags&PASSIVE_MODE) && state==DATASOCKET_CONNECTING_STATE
           && (pasv_state==PASV_NO_ADDRESS_YET || pasv_state==PASV_HAVE_ADDRESS)))
	 DataClose();	// just close data connection
      else
      {
	 // otherwise, just close control connection.
	 conn->quit_sent=true;
	 Disconnect();
      }
      return;
   }

   if(conn->aborted_data_sock!=-1)  // don't allow double ABOR.
   {
      conn->quit_sent=true;
      Disconnect();
      return;
   }

   SendUrgentCmd("ABOR");
   AddResp(226,CHECK_ABOR);
   FlushSendQueue(true);
   AbortedClose();
   // don't close it now, wait for ABOR result
   conn->aborted_data_sock=conn->data_sock;
   conn->data_sock=-1;
   Delete(conn->data_iobuf);
   conn->data_iobuf=0;

   // ABOR over SSL connection does not always work,
   // closing data socket should help it.
   if(conn->control_ssl)
      AbortedClose();

   if(QueryBool("web-mode"))
      Disconnect();
}

void Ftp::ControlClose()
{
   delete conn; conn=0;
   EmptyRespQueue();
}

void Ftp::AbortedClose()
{
   if(conn && conn->aborted_data_sock!=-1)
   {
      DebugPrint("---- ",_("Closing aborted data socket"));
      close(conn->aborted_data_sock);
      conn->aborted_data_sock=-1;
   }
}

void  Ftp::Disconnect()
{
   /* protect against re-entering from FlushSendQueue */
   static bool disconnect_in_progress=false;
   if(disconnect_in_progress)
      return;
   disconnect_in_progress=true;

   bool no_greeting=(!RespQueueIsEmpty() && RespQueue[RQ_head].check_case==CHECK_READY);

   CloseRespQueue();
   DataAbort();
   DataClose();
   if(conn && state!=CONNECTING_STATE && !conn->quit_sent
   && RespQueueSize()<2 && QueryBool("use-quit",hostname))
   {
      SendCmd("QUIT");
      AddResp(221);
      conn->quit_sent=true;
      goto out;
   }
   ControlClose();
   AbortedClose();

   if(state==CONNECTING_STATE || no_greeting)
      NextPeer();

   if(copy_mode!=COPY_NONE)
   {
      if(copy_addr_valid)
	 copy_failed=true;
   }
   else
   {
      if(mode==STORE && (flags&IO_FLAG))
	 SetError(STORE_FAILED,0);
   }
   state=INITIAL_STATE;
   http_proxy_status_code=0;

out:
   disconnect_on_close=false;
   Timeout(0);

   disconnect_in_progress=false;
}

void Ftp::Connection::CloseDataConnection()
{
   Delete(data_iobuf); data_iobuf=0;
   if(data_sock!=-1)
   {
      Log::global->Format(7,"---- %s\n",_("Closing data socket"));
      close(data_sock);
      data_sock=-1;
   }
   fixed_pasv=false;
}

void  Ftp::DataClose()
{
   if(!conn)
      return;
   if(conn->data_sock!=-1 && QueryBool("web-mode"))
      disconnect_on_close=true;
   conn->CloseDataConnection();
   nop_time=0;
   nop_offset=0;
   nop_count=0;
   if(rate_limit)
   {
      delete rate_limit;
      rate_limit=0;
   }
   if(state==DATA_OPEN_STATE || state==DATASOCKET_CONNECTING_STATE)
      state=WAITING_STATE;
}

int Ftp::FlushSendQueueOneCmd()
{
   const char *send_cmd_ptr;
   int send_cmd_count;
   conn->send_cmd_buffer->Get(&send_cmd_ptr,&send_cmd_count);

   if(send_cmd_count==0)
      return STALL;

   const char *cmd_begin=send_cmd_ptr;
   const char *line_end=(const char*)memchr(send_cmd_ptr,'\n',send_cmd_count);
   if(!line_end)
      return STALL;

   int to_write=line_end+1-send_cmd_ptr;
   conn->control_send->Put(send_cmd_ptr,to_write);
   conn->send_cmd_buffer->Skip(to_write);
   conn->sync_wait++;

   int log_level=5;

   bool may_show = (skey_pass!=0) || (user==0) || pass_open;
   if(proxy && proxy_user) // can't distinguish here
      may_show=false;
   if(!may_show && !strncasecmp(cmd_begin,"PASS ",5))
      Log::global->Write(log_level,"---> PASS XXXX\n");
   else
   {
      Log::global->Write(log_level,"---> ");
      for(const char *s=cmd_begin; s<=line_end; s++)
      {
	 if(*s==0)
	    Log::global->Write(log_level,"<NUL>");
	 else if((unsigned char)*s==TELNET_IAC && use_telnet_iac)
	 {
	    s++;
	    if((unsigned char)*s==TELNET_IAC)
	       Log::global->Write(log_level,"\377");
	    else if((unsigned char)*s==TELNET_IP)
	       Log::global->Write(log_level,"<IP>");
	    else if((unsigned char)*s==TELNET_DM)
	       Log::global->Write(log_level,"<DM>");
	 }
	 else
	    Log::global->Format(log_level,"%c",*s?*s:'!');
      }
   }
   return MOVED;
}

int  Ftp::FlushSendQueue(bool all)
{
   int m=STALL;

   if(!conn || !conn->control_send)
      return m;

   BumpEventTime(conn->control_send->EventTime());
   if(conn->control_send->Error())
   {
      DebugPrint("**** ",conn->control_send->ErrorText(),0);
      if(conn->control_send->ErrorFatal())
	 SetError(FATAL,conn->control_send->ErrorText());
      conn->quit_sent=true;
      Disconnect();
      return MOVED;
   }

   while(conn->sync_wait<=0 || all || !(flags&SYNC_MODE))
   {
      int res=FlushSendQueueOneCmd();
      if(res==STALL)
	 break;
      m|=res;
   }

   if(m==MOVED)
      Roll(conn->control_send);

   return m;
}

void  Ftp::Send(const char *buf,int len)
{
   while(len>0)
   {
      char ch=*buf++;
      len--;
      if(ch=='\377' && use_telnet_iac) // double chr(255) as in telnet protocol
      	 conn->send_cmd_buffer->Put("\377",1);
      conn->send_cmd_buffer->Put(&ch,1);
      if(ch=='\r')
	 conn->send_cmd_buffer->Put("",1); // RFC2640
   }
}

void  Ftp::SendCmd(const char *cmd)
{
   Send(cmd,strlen(cmd));
   conn->send_cmd_buffer->Put("\r\n",2);
}

void Ftp::SendCmd2(const char *cmd,const char *f)
{
   if(cmd && cmd[0])
   {
      Send(cmd,strlen(cmd));
      conn->send_cmd_buffer->Put(" ",1);
   }
   Send(f,strlen(f));
   conn->send_cmd_buffer->Put("\r\n",2);
}

void Ftp::SendCmd2(const char *cmd,int v)
{
   char buf[32];
   sprintf(buf,"%d",v);
   SendCmd2(cmd,buf);
}

int   Ftp::SendEOT()
{
   if(mode!=STORE)
      return(OK); /* nothing to do */

   if(state!=DATA_OPEN_STATE)
      return(DO_AGAIN);

   if(!conn->data_iobuf->Eof())
      conn->data_iobuf->PutEOF();

   if(conn->data_iobuf->Size()>0)
      return(DO_AGAIN);

   DataClose();
   state=WAITING_STATE;
   return(OK);
}

void  Ftp::Close()
{
   if(mode!=CLOSED)
      set_idle_start();

   flags&=~NOREST_MODE;	// can depend on a particular file
   eof=false;

   Resume();
   ExpandTildeInCWD();
   DataAbort();
   DataClose();
   if(conn)
   {
      switch(state)
      {
      case(CONNECTING_STATE):
      case(HTTP_PROXY_CONNECTED):
      case(CONNECTED_STATE):
      case(USER_RESP_WAITING_STATE):
	 Disconnect();
	 break;
      case(ACCEPTING_STATE):
      case(DATASOCKET_CONNECTING_STATE):
      case(CWD_CWD_WAITING_STATE):
      case(WAITING_STATE):
      case(DATA_OPEN_STATE):
	 state=(conn->control_sock==-1 ? INITIAL_STATE : EOF_STATE);
	 break;
      case(INITIAL_STATE):
      case(EOF_STATE):
	 break;
      }
   }
   else
   {
      state=INITIAL_STATE;
   }
   copy_mode=COPY_NONE;
   copy_addr_valid=false;
   copy_done=false;
   copy_connection_open=false;
   stat_time=0;
   copy_allow_store=false;
   copy_failed=false;
   CloseRespQueue();
   super::Close();
   if(disconnect_on_close)
      Disconnect();
}

bool Ftp::RespQueueHas(check_case_t cc)
{
   for(int i=RQ_head; i<RQ_tail; i++)
      if(cc==RespQueue[i].check_case)
	 return true;
   return false;
}

void Ftp::CloseRespQueue()
{
   for(int i=RQ_head; i<RQ_tail; i++)
   {
      check_case_t cc=RespQueue[i].check_case;
      switch(cc)
      {
      case(CHECK_IGNORE):
      case(CHECK_PWD):
      case(CHECK_USER):
      case(CHECK_USER_PROXY):
      case(CHECK_PASS):
      case(CHECK_PASS_PROXY):
      case(CHECK_READY):
      case(CHECK_ABOR):
      case(CHECK_CWD_STALE):
      case(CHECK_PRET):
      case(CHECK_PASV):
      case(CHECK_EPSV):
      case(CHECK_TRANSFER_CLOSED):
      case(CHECK_FEAT):
      case(CHECK_SITE_UTIME):
#ifdef USE_SSL
      case(CHECK_AUTH_TLS):
      case(CHECK_PROT):
#endif
	 break;
      case(CHECK_CWD_CURR):
      case(CHECK_CWD):
	 if(RespQueue[i].path==0)
	 {
	    Disconnect();
	    return;  // can't Close() with this in queue
	 }
	 RespQueue[i].check_case=CHECK_CWD_STALE;
	 break;
      case(CHECK_NONE):
      case(CHECK_REST):
      case(CHECK_SIZE):
      case(CHECK_SIZE_OPT):
      case(CHECK_MDTM):
      case(CHECK_MDTM_OPT):
      case(CHECK_PORT):
      case(CHECK_FILE_ACCESS):
      case(CHECK_RNFR):
      case(CHECK_QUOTED):
	 RespQueue[i].check_case=CHECK_IGNORE;
	 break;
      case(CHECK_TRANSFER):
	 RespQueue[i].check_case=CHECK_TRANSFER_CLOSED;
	 break;
      }
   }
}

Ftp::expected_response *Ftp::FindLastCWD()
{
   for(int i=RQ_tail-1; i>=RQ_head; i--)
   {
      switch(RespQueue[i].check_case)
      {
      case(CHECK_CWD_CURR):
      case(CHECK_CWD_STALE):
      case(CHECK_CWD):
	 return &RespQueue[i];
      default:
	 ;
      }
   }
   return 0;
}

bool  Ftp::IOReady()
{
   if(copy_mode!=COPY_NONE && !copy_passive && !copy_addr_valid)
      return true;   // simulate to be ready as other fxp peer has to go
   return (state==DATA_OPEN_STATE || state==WAITING_STATE)
      && real_pos!=-1 && IsOpen();
}

int   Ftp::Read(void *buf,int size)
{
   int shift;

   Resume();
   if(Error())
      return(error_code);

   if(mode==CLOSED || eof)
      return(0);

   if(!conn || !conn->data_iobuf)
      return DO_AGAIN;

   if(RespQueueSize()>1 && real_pos==-1)
      return DO_AGAIN;

   if(state==DATA_OPEN_STATE)
   {
      assert(rate_limit!=0);
      int allowed=rate_limit->BytesAllowedToGet();
      if(allowed==0)
	 return DO_AGAIN;
      if(size>allowed)
	 size=allowed;
   }
   if(norest_manual && real_pos==0 && pos>0)
      return DO_AGAIN;

   const char *b;
   int s;
   conn->data_iobuf->Get(&b,&s);
   if(s==0)
      return DO_AGAIN;
   if(size>s)
      size=s;
   memcpy(buf,b,size);
   conn->data_iobuf->Skip(size);

   retries=0;
   persist_retries=0;
   assert(rate_limit!=0);
   rate_limit->BytesGot(size);
   real_pos+=size;
   if(real_pos<=pos)
      return DO_AGAIN;
   flags|=IO_FLAG;
   if((shift=pos+size-real_pos)>0)
   {
      memmove(buf,(char*)buf+shift,size-shift);
      size-=shift;
   }
   pos+=size;
   return(size);
}

/*
   Write - send data to ftp server

   * Uploading is not reliable in this realization *
   Well, not less reliable than in any usual ftp client.

   The reason for this is uncheckable receiving of data on the remote end.
   Since that, we have to leave re-putting up to caller.
   Fortunately, class FileCopy does it.
*/
int   Ftp::Write(const void *buf,int size)
{
   if(mode!=STORE)
      return(0);

   Resume();
   if(Error())
      return(error_code);

   if(!conn || state!=DATA_OPEN_STATE || (RespQueueSize()>1 && real_pos==-1) || !conn->data_iobuf)
      return DO_AGAIN;

   {
      assert(rate_limit!=0);
      int allowed=rate_limit->BytesAllowedToPut();
      if(allowed==0)
	 return DO_AGAIN;
      if(size>allowed)
	 size=allowed;
   }
   if(size+conn->data_iobuf->Size()>=max_buf)
      size=max_buf-conn->data_iobuf->Size();
   if(size<=0)
      return 0;

   conn->data_iobuf->Put((const char*)buf,size);

   if(retries+persist_retries>0
   && conn->data_iobuf->GetPos()-conn->data_iobuf->Size()>Buffered()+0x1000)
   {
      // reset retry count if some data were actually written to server.
      retries=0;
      persist_retries=0;
   }

   assert(rate_limit!=0);
   rate_limit->BytesPut(size);
   pos+=size;
   real_pos+=size;
   flags|=IO_FLAG;
   return(size);
}

int   Ftp::StoreStatus()
{
   if(Error())
      return(error_code);

   if(mode!=STORE)
      return(OK);

   if(state==DATA_OPEN_STATE)
   {
      // have not send EOT by SendEOT, do it now
      SendEOT();
   }

   if(state==WAITING_STATE && RespQueueIsEmpty())
   {
      eof=true;
      return(OK);
   }

   return(IN_PROGRESS);
}

void  Ftp::AddResp(int exp,check_case_t ck)
{
   int newtail=RQ_tail+1;
   if(newtail>RQ_alloc)
   {
      if(RQ_head-0<newtail-RQ_alloc)
	 RespQueue=(struct expected_response*)
	    xrealloc(RespQueue,(RQ_alloc=newtail+16)*sizeof(*RespQueue));
      memmove(RespQueue,RespQueue+RQ_head,(RQ_tail-RQ_head)*sizeof(*RespQueue));
      RQ_tail=0+(RQ_tail-RQ_head);
      RQ_head=0;
      newtail=RQ_tail+1;
   }
   RespQueue[RQ_tail].expect=exp;
   RespQueue[RQ_tail].check_case=ck;
   RespQueue[RQ_tail].path=0;
   RQ_tail=newtail;
}

void  Ftp::EmptyRespQueue()
{
   while(!RespQueueIsEmpty())
      PopResp();
   RQ_head=RQ_tail=0;
   xfree(RespQueue);
   RespQueue=0;
   RQ_alloc=0;
}
void  Ftp::MoveConnectionHere(Ftp *o)
{
   assert(conn==0);

   RQ_head=o->RQ_head; o->RQ_head=0;
   RQ_tail=o->RQ_tail; o->RQ_tail=0;
   RespQueue=o->RespQueue; o->RespQueue=0;
   RQ_alloc=o->RQ_alloc; o->RQ_alloc=0;

   CloseRespQueue(); // we need not handle other session's replies.

   o->state=INITIAL_STATE;
   assert(!conn);
   conn=o->conn;
   o->conn=0;
   if(peer_curr>=peer_num)
      peer_curr=0;
   type=o->type;
   event_time=o->event_time;

#ifdef USE_SSL
   auth_supported=o->auth_supported;
   auth_args_supported=o->auth_args_supported; o->auth_args_supported=0;
#endif

   size_supported=o->size_supported;
   mdtm_supported=o->mdtm_supported;
   rest_supported=o->rest_supported;
   site_chmod_supported=o->site_chmod_supported;
   site_utime_supported=o->site_utime_supported;
   pret_supported=o->pret_supported;
   utf8_supported=o->utf8_supported;
   lang_supported=o->lang_supported;
   last_rest=o->last_rest;
   proxy_is_http=o->proxy_is_http;

   if(!home)
      set_home(home_auto);

   set_real_cwd(o->real_cwd);
   o->set_real_cwd(0);
   o->Disconnect();
   state=EOF_STATE;
}

void Ftp::CheckFEAT(char *reply)
{
   pret_supported=false;
   mdtm_supported=false;
   size_supported=false;
   rest_supported=false;
#ifdef USE_SSL
   auth_supported=false;
#endif

   char *scan=strchr(reply,'\n');
   if(scan)
      scan++;
   if(!scan || !*scan)
      return;

   for(char *f=strtok(scan,"\r\n"); f; f=strtok(0,"\r\n"))
   {
      if(!strncmp(f,"211 ",4))
	 break;	  // last line
      if(*f==' ')
	 f++;
      if(!strcasecmp(f,"UTF8"))
      {
	 utf8_supported=true;
	 conn->SetControlConnectionTranslation("UTF-8");
	 // some non-RFC2640 servers require this command.
	 SendCmd("OPTS UTF8 ON");
	 AddResp(0,CHECK_IGNORE);
      }
      else if(!strncasecmp(f,"LANG ",5))
      {
	 lang_supported=true;
	 const char *lang_to_use=Query("lang",hostname);
	 if(lang_to_use && lang_to_use[0])
	    SendCmd2("LANG",lang_to_use);
	 else
	    SendCmd("LANG");
	 AddResp(200,CHECK_IGNORE);
      }
      else if(!strcasecmp(f,"PRET"))
	 pret_supported=true;
      else if(!strcasecmp(f,"MDTM"))
	 mdtm_supported=true;
      else if(!strcasecmp(f,"SIZE"))
	 size_supported=true;
      else if(!strncasecmp(f,"REST ",5))
	 rest_supported=true;
#ifdef USE_SSL
      else if(!strncasecmp(f,"AUTH ",5))
      {
	 auth_supported=true;
	 xfree(auth_args_supported);
	 auth_args_supported=xstrdup(f+5);
      }
#endif // USE_SSL
   }
}

void Ftp::CheckResp(int act)
{
   if(act==150 && flags&PASSIVE_MODE && conn->aborted_data_sock!=-1)
      AbortedClose();

   if(act==150 && state==WAITING_STATE && RespQueueSize()==1)
   {
      copy_connection_open=true;
      stat_time=now+2;
   }

   if(act==150 && mode==RETRIEVE && opt_size && *opt_size<0)
   {
      // try to catch size
      char *s=strrchr(line,'(');
      if(s && is_ascii_digit(s[1]))
      {
	 long long size_ll;
	 if(1==sscanf(s+1,"%lld",&size_ll))
	 {
	    *opt_size=size_ll;
	    DebugPrint("---- ",_("saw file size in response"),7);
	 }
      }
   }

   if(is1XX(act)) // intermediate responses are ignored
      return;

   if(act==421 || act==221)  // timeout or something else
   {
      if(RespQueueIsEmpty() || RespQueue[RQ_head].expect!=221)
	 DebugPrint("**** ",_("remote end closed connection"),3);
      conn->quit_sent=true;
      Disconnect();
      return;
   }

   if(RespQueueIsEmpty())
   {
      DebugPrint("**** ",_("extra server response"),3);
      if(is2XX(act)) // some buggy servers send several 226 replies
	 return;     // ignore them.
      Disconnect();
      return;
   }

   int exp=RespQueue[RQ_head].expect;

   // some servers mess all up
   if(act==331 && exp==220 && !(flags&SYNC_MODE) && RespQueueSize()>1)
   {
      PopResp();
      DebugPrint("---- ",_("Turning on sync-mode"),2);
      ResMgr::Set("ftp:sync-mode",hostname,"on");
      Disconnect();
      try_time=0; // retry immediately
      return;
   }

   bool match=(act/100==exp/100);
   check_case_t cc=RespQueue[RQ_head].check_case;
   const char *exp_path=alloca_strdup(RespQueue[RQ_head].path);

   // remove first, so that Disconnect() has accurate picture.
   PopResp();

   switch(cc)
   {
   case CHECK_NONE:
      if(match) // default rule.
	 break;
      Disconnect();
      break;

   case CHECK_IGNORE:
   case CHECK_QUOTED:
   ignore:
      break;

   case CHECK_READY:
      if(!(flags&SYNC_MODE) && re_match(all_lines,Query("auto-sync-mode",hostname)))
      {
	 DebugPrint("---- ",_("Turning on sync-mode"),2);
	 ResMgr::Set("ftp:sync-mode",hostname,"on");
	 assert(flags&SYNC_MODE);
	 Disconnect();
	 try_time=0; // retry immediately
      }
      if(!match)
      {
	 Disconnect();
	 NextPeer();
	 if(peer_curr==0)
	    try_time=now;  // count the reconnect-interval from this moment
      }
      break;

   case CHECK_REST:
      RestCheck(act);
      break;

   case CHECK_CWD:
   case CHECK_CWD_CURR:
      if(is2XX(act))
      {
	 if(cc==CHECK_CWD)
	 {
	    xfree(cwd);
	    cwd=xstrdup(exp_path);
	 }
	 set_real_cwd(cwd);
	 LsCache::SetDirectory(this, exp_path, true);
	 break;
      }
      if(is5XX(act))
      {
	 SetError(NO_FILE,all_lines);
	 LsCache::SetDirectory(this, exp_path, false);
	 break;
      }
      Disconnect();
      break;

   case CHECK_CWD_STALE:
      if(is2XX(act))
	 set_real_cwd(exp_path);
      goto ignore;

   case CHECK_ABOR:
      AbortedClose();
      goto ignore;

   case CHECK_SIZE:
      CatchSIZE(act);
      break;
   case CHECK_SIZE_OPT:
      CatchSIZE_opt(act);
      break;
   case CHECK_MDTM:
      CatchDATE(act);
      break;
   case CHECK_MDTM_OPT:
      CatchDATE_opt(act);
      break;

   case CHECK_FILE_ACCESS:
   file_access:
      if(mode==CHANGE_MODE && (act==500 || act==501 || act==502))
      {
	 site_chmod_supported=false;
	 SetError(NO_FILE,all_lines);
	 break;
      }
      NoFileCheck(act);
      break;

   case CHECK_PRET:
      if(act==500 || act==502)
	 pret_supported=false;
      goto ignore;

   case CHECK_PASV:
   case CHECK_EPSV:
      if(is2XX(act))
      {
	 if(strlen(line)<=4)
	    goto passive_off;

	 memset(&data_sa,0,sizeof(data_sa));

	 if(cc==CHECK_PASV)
	    pasv_state=Handle_PASV();
	 else // cc==CHECK_EPSV
	    pasv_state=Handle_EPSV();

	 if(pasv_state==PASV_NO_ADDRESS_YET)
	    goto passive_off;

	 if(conn->aborted_data_sock!=-1)
	    SocketConnect(conn->aborted_data_sock,&data_sa);

      	 break;
      }
      if(copy_mode!=COPY_NONE)
      {
	 copy_passive=!copy_passive;
	 Disconnect();
	 break;
      }
      if(is5XX(act))
      {
      passive_off:
	 DebugPrint("---- ",_("Switching passive mode off"),2);
	 SetFlag(PASSIVE_MODE,0);
      }
      Disconnect();
      break;

   case CHECK_PORT:
      if(is2XX(act))
	 break;
      if(copy_mode!=COPY_NONE)
      {
	 copy_passive=!copy_passive;
	 Disconnect();
	 break;
      }
      if(is5XX(act))
      {
	 DebugPrint("---- ",_("Switching passive mode on"),2);
	 SetFlag(PASSIVE_MODE,1);
      }
      Disconnect();
      break;

   case CHECK_PWD:
      if(is2XX(act))
      {
	 if(!home_auto)
	 {
	    home_auto=ExtractPWD();   // it allocates space.
	    PropagateHomeAuto();
	 }
	 if(!home)
	    set_home(home_auto);
	 LsCache::SetDirectory(this, home, true);
	 break;
      }
      break;

   case CHECK_RNFR:
      if(is3XX(act))
      {
	 SendCmd2("RNTO",file1);
	 AddResp(250,CHECK_FILE_ACCESS);
      	 break;
      }
      goto file_access;

   case CHECK_USER_PROXY:
      proxy_NoPassReqCheck(act);
      break;
   case CHECK_USER:
      NoPassReqCheck(act);
      break;
   case CHECK_PASS_PROXY:
      proxy_LoginCheck(act);
      break;
   case CHECK_PASS:
      LoginCheck(act);
      break;

   case CHECK_TRANSFER:
      TransferCheck(act);
      if(mode==STORE && is2XX(act)
      && entity_size==real_pos)
	 SendUTimeRequest();
      break;

   case CHECK_TRANSFER_CLOSED:
      if(strstr(line,"ABOR")
      && RespQueueSize()>=2 && RespQueue[RQ_head].check_case==CHECK_ABOR)
      {
	 DebugPrint("**** ","server bug: 426 reply missed",1);
	 PopResp();
	 AbortedClose();
      }
      break;
   case CHECK_FEAT:
      if(act==530)
	 try_feat_after_login=true;
      if(!is2XX(act))
	 break;
      CheckFEAT(all_lines);
      break;

   case CHECK_SITE_UTIME:
      if(act==500 || act==501 || act==502)
      {
	 site_utime_supported=false;
	 SendUTimeRequest();  // try another method.
      }
      break;

#ifdef USE_SSL
   case CHECK_AUTH_TLS:
      if(is2XX(act))
      {
	 conn->MakeSSLBuffers(hostname);
      }
      else
      {
	 if(QueryBool("ssl-force",hostname))
	    SetError(LOGIN_FAILED,_("ftp:ssl-force is set and server does not support or allow SSL"));
      }
      break;
   case CHECK_PROT:
      if(is2XX(act))
	 conn->prot^='C'^'P';
      break;
#endif // USE_SSL

   } /* end switch */
}

void  Ftp::SetRespPath(const char *p)
{
   if(RQ_tail>RQ_head)
   {
      xfree(RespQueue[RQ_tail-1].path);
      RespQueue[RQ_tail-1].path=xstrdup(p);
   }
}

void  Ftp::PopResp()
{
   if(RQ_head!=RQ_tail)
   {
      xfree(RespQueue[RQ_head].path);
      RQ_head=RQ_head+1;
   }
}

const char *Ftp::CurrentStatus()
{
   if(Error())
      return StrError(error_code);
   if(RespQueueHas(CHECK_FEAT))
      return _("FEAT negotiation...");
   switch(state)
   {
   case(EOF_STATE):
      if(conn && conn->control_sock!=-1)
      {
	 if(conn->send_cmd_buffer->Size()>0)
	    return(_("Sending commands..."));
	 if(!RespQueueIsEmpty())
	    return(_("Waiting for response..."));
	 if(retry_time>now)
	    return _("Delaying before retry");
	 return(_("Connection idle"));
      }
      return(_("Not connected"));
   case(INITIAL_STATE):
      if(hostname)
      {
	 if(resolver)
	    return(_("Resolving host address..."));
	 if(!ReconnectAllowed())
	    return DelayingMessage();
      }
      return(_("Not connected"));
   case(CONNECTING_STATE):
   case(HTTP_PROXY_CONNECTED):
      return(_("Connecting..."));
   case(CONNECTED_STATE):
#ifdef USE_SSL
      if(conn->auth_sent)
	 return _("TLS negotiation...");
#endif
      return _("Connected");
   case(USER_RESP_WAITING_STATE):
      return(_("Logging in..."));
   case(DATASOCKET_CONNECTING_STATE):
      if(pasv_state==PASV_NO_ADDRESS_YET)
	 return(_("Waiting for response..."));
      return(_("Making data connection..."));
   case(CWD_CWD_WAITING_STATE):
      if(FindLastCWD())
	 return(_("Changing remote directory..."));
      /*fallthrough*/
   case(WAITING_STATE):
      if(copy_mode==COPY_SOURCE)
	 return "";
      if(copy_mode==COPY_DEST && RespQueueIsEmpty())
	 return _("Waiting for other copy peer...");
      if(mode==STORE)
	 return(_("Waiting for transfer to complete"));
      return(_("Waiting for response..."));
   case(ACCEPTING_STATE):
      return(_("Waiting for data connection..."));
   case(DATA_OPEN_STATE):
#ifdef USE_SSL
      if(conn->data_ssl)
      {
	 if(mode==STORE)
	    return(_("Sending data/TLS"));
         else
	    return(_("Receiving data/TLS"));
      }
#endif
      if(conn->data_sock!=-1)
      {
	 if(mode==STORE)
	    return(_("Sending data"));
         else
	    return(_("Receiving data"));
      }
      return(_("Waiting for transfer to complete"));
   }
   abort();
}

time_t	 Ftp::ConvertFtpDate(const char *s)
{
   struct tm tm;
   memset(&tm,0,sizeof(tm));
   int year,month,day,hour,minute,second;

   int skip=0;
   int n=sscanf(s,"%4d%n",&year,&skip);

   if(n==1 && year==1910)  // try to workaround server's y2k bug
   {
      n=sscanf(s,"%5d%n",&year,&skip);
      if(year>=19100)
	 year=year-19100+2000;
   }

   if(n!=1)
      return NO_DATE;

   n=sscanf(s+skip,"%2d%2d%2d%2d%2d",&month,&day,&hour,&minute,&second);

   if(n!=5)
      return NO_DATE;

   tm.tm_year=year-1900;
   tm.tm_mon=month-1;
   tm.tm_mday=day;
   tm.tm_hour=hour;
   tm.tm_min=minute;
   tm.tm_sec=second;

   return mktime_from_utc(&tm);
}

void  Ftp::SetFlag(int flag,bool val)
{
   flag&=MODES_MASK;  // only certain flags can be changed
   if(val)
      flags|=flag;
   else
      flags&=~flag;
}

bool  Ftp::SameSiteAs(FileAccess *fa)
{
   if(!SameProtoAs(fa))
      return false;
   Ftp *o=(Ftp*)fa;
   return(!xstrcasecmp(hostname,o->hostname) && !xstrcmp(portname,o->portname)
   && !xstrcmp(user,o->user) && !xstrcmp(pass,o->pass)
   && ftps==o->ftps);
}

bool  Ftp::SameConnection(const Ftp *o)
{
   if(!strcasecmp(hostname,o->hostname) && !xstrcmp(portname,o->portname)
   && !xstrcmp(user,o->user) && !xstrcmp(pass,o->pass)
   && (user || !xstrcmp(anon_user,o->anon_user))
   && (pass || !xstrcmp(anon_pass,o->anon_pass))
   && ftps==o->ftps)
      return true;
   return false;
}

bool  Ftp::SameLocationAs(FileAccess *fa)
{
   if(!SameProtoAs(fa))
      return false;
   Ftp *o=(Ftp*)fa;
   if(!hostname || !o->hostname)
      return false;
   if(SameConnection(o))
   {
      if(home && o->home && strcmp(home,o->home))
	 return false;

      if(!cwd || !o->cwd)
	 return false;

      ExpandTildeInCWD();
      o->ExpandTildeInCWD();
      return !xstrcmp(cwd,o->cwd);
   }
   return false;
}

int   Ftp::Done()
{
   Resume();

   if(Error())
      return(error_code);

   if(mode==CLOSED)
      return OK;

   if(mode==ARRAY_INFO)
   {
      if(state==WAITING_STATE && RespQueueIsEmpty() && array_ptr==array_cnt)
	 return(OK);
      return(IN_PROGRESS);
   }

   if(copy_mode==COPY_DEST && !copy_allow_store)
      return(IN_PROGRESS);

   if(mode==CHANGE_DIR || mode==RENAME
   || mode==MAKE_DIR || mode==REMOVE_DIR || mode==REMOVE || mode==CHANGE_MODE
   || copy_mode!=COPY_NONE)
   {
      if(state==WAITING_STATE && RespQueueIsEmpty())
	 return(OK);
      return(IN_PROGRESS);
   }
   if(mode==CONNECT_VERIFY)
   {
      if(state!=INITIAL_STATE)
	 return OK;
      return(peer?OK:IN_PROGRESS);
   }
   abort();
}

void Ftp::ResetLocationData()
{
   super::ResetLocationData();
   flags=0;
   dos_path=false;
   vms_path=false;
   mdtm_supported=true;
   size_supported=true;
   rest_supported=true;
   site_chmod_supported=true;
   site_utime_supported=true;
   utf8_supported=false;
   lang_supported=false;
   pret_supported=false;
#ifdef USE_SSL
   auth_supported=true;
   xfree(auth_args_supported);
   auth_args_supported=0;
#endif
   xfree(home_auto); home_auto=0;
   home_auto=xstrdup(FindHomeAuto());
   Reconfig(0);
   state=INITIAL_STATE;
}

void Ftp::Reconfig(const char *name)
{
   xfree(closure);
   closure=xstrdup(hostname);

   super::Reconfig(name);

   if(!xstrcmp(name,"net:idle") || !xstrcmp(name,"ftp:use-site-idle"))
   {
      if(conn && conn->data_sock==-1 && state==EOF_STATE && !conn->quit_sent)
	 SendSiteIdle();
      return;
   }

   const char *c=closure;

   SetFlag(SYNC_MODE,	QueryBool("sync-mode",c));
   SetFlag(PASSIVE_MODE,QueryBool("passive-mode",c));
   rest_list = QueryBool("rest-list",c);

   nop_interval = Query("nop-interval",c);

   allow_skey = QueryBool("skey-allow",c);
   force_skey = QueryBool("skey-force",c);
   verify_data_address = QueryBool("verify-address",c);
   verify_data_port = QueryBool("verify-port",c);

   use_stat = QueryBool("use-stat",c);
   stat_interval = Query("stat-interval",c);

   use_mdtm = QueryBool("use-mdtm",c);
   use_size = QueryBool("use-size",c);
   use_pret = QueryBool("use-pret",c);
   use_feat = QueryBool("use-feat",c);

   use_telnet_iac = QueryBool("use-telnet-iac",c);

   xfree(list_options);
   list_options = xstrdup(Query("list-options",c));

   xfree(anon_user);
   anon_user=xstrdup(Query("anon-user",c));
   xfree(anon_pass);
   anon_pass=xstrdup(Query("anon-pass",c));

   xfree(charset);
   charset=xstrdup(Query("charset",c));
   if(conn && !utf8_supported && charset && *charset && !strcmp(name,"ftp:charset"))
      conn->SetControlConnectionTranslation(charset);

   const char *h=QueryStringWithUserAtHost("home");
   if(h && h[0] && AbsolutePath(h))
      set_home(h);
   else
      set_home(home_auto);

   if(NoProxy(hostname))
      SetProxy(0);
   else
      SetProxy(Query("proxy",c));

   if(proxy && proxy_port==0)
   {
      if(ProxyIsHttp())
	 proxy_port=xstrdup(HTTP_DEFAULT_PROXY_PORT);
      else
	 proxy_port=xstrdup(FTP_DEFAULT_PORT);
   }

   if(nop_interval<30)
      nop_interval=30;

   if(conn && conn->control_sock!=-1)
      SetSocketBuffer(conn->control_sock);
   if(conn && conn->data_sock!=-1)
      SetSocketBuffer(conn->data_sock);
}

void Ftp::Cleanup()
{
   if(hostname==0)
      return;

   for(FA *fo=FirstSameSite(); fo!=0; fo=NextSameSite(fo))
      fo->CleanupThis();

   CleanupThis();
}
void Ftp::CleanupThis()
{
   if(!conn || mode!=CLOSED)
      return;
   Disconnect();
}

void Ftp::SetError(int ec,const char *e)
{
   switch((status)ec)
   {
   case(SEE_ERRNO):
   case(LOOKUP_ERROR):
   case(NO_HOST):
   case(FATAL):
   case(LOGIN_FAILED):
      Disconnect();
      break;
   case(IN_PROGRESS):
   case(OK):
   case(NOT_OPEN):
   case(NO_FILE):
   case(FILE_MOVED):
   case(STORE_FAILED):
   case(DO_AGAIN):
   case(NOT_SUPP):
      break;
   }
   // join multiline error message into single line, removing `550-' prefix.
   if(e && strchr(e,'\n'))
   {
      char *joined=string_alloca(strlen(e)+1);
      const char *prefix=e;
      char *store=joined;
      while(*e)
      {
	 if(*e=='\n')
	 {
	    if(e[1])
	       *store++=' ';
	    e++;
	    if(!strncmp(e,prefix,3) && (e[3]=='-' || e[3]==' '))
	       e+=4;
	 }
	 else
	 {
	    *store++=*e++;
	 }
      }
      *store=0;
      e=joined;
   }
   super::SetError(ec,e);
}

Ftp::ConnectLevel Ftp::GetConnectLevel()
{
   if(!conn)
      return CL_NOT_CONNECTED;
   if(state==CONNECTING_STATE || state==HTTP_PROXY_CONNECTED)
      return CL_CONNECTING;
   if(state==CONNECTED_STATE)
      return CL_JUST_CONNECTED;
   if(state==USER_RESP_WAITING_STATE)
      return CL_NOT_LOGGED_IN;
   if(conn->quit_sent)
      return CL_JUST_BEFORE_DISCONNECT;
   return CL_LOGGED_IN;
}

ListInfo *Ftp::MakeListInfo(const char *path)
{
   return new FtpListInfo(this,path);
}
Glob *Ftp::MakeGlob(const char *pattern)
{
   return new GenericGlob(this,pattern);
}
DirList *Ftp::MakeDirList(ArgV *args)
{
   return new FtpDirList(args,this);
}


extern "C"
   const char *calculate_skey_response (int, const char *, const char *);

const char *Ftp::make_skey_reply()
{
   static const char * const skey_head[] = {
      "S/Key MD5 ",
      "s/key ",
      "opiekey ",
      "otp-md5 ",
      0
   };

   const char *cp;
   for(int i=0; ; i++)
   {
      if(skey_head[i]==0)
	 return 0;
      cp=strstr(all_lines,skey_head[i]);
      if(cp)
      {
	 cp+=strlen(skey_head[i]);
	 break;
      }
   }

   DebugPrint("---- ","found s/key substring");

   int skey_sequence=0;
   char *buf=(char*)alloca(strlen(cp));

   if(sscanf(cp,"%d %s",&skey_sequence,buf)!=2 || skey_sequence<1)
      return 0;

   return calculate_skey_response(skey_sequence,buf,pass);
}

int Ftp::Buffered()
{
   if(!conn->data_iobuf)
      return 0;
   if(state!=DATA_OPEN_STATE || conn->data_sock==-1 || mode!=STORE)
      return 0;
   return conn->data_iobuf->Size()+SocketBuffered(conn->data_sock);
}

const char *Ftp::ProtocolSubstitution(const char *host)
{
   if(NoProxy(host))
      return 0;
   const char *proxy=ResMgr::Query("ftp:proxy",host);
   if(proxy && QueryBool("use-hftp",host)
   && (!strncmp(proxy,"http://",7) || !strncmp(proxy,"https://",8)))
      return "hftp";
   return 0;
}


#ifdef USE_SSL
#undef super
#define super Ftp
FtpS::FtpS()
{
   ftps=true;
   res_prefix="ftp";
}
FtpS::~FtpS()
{
}
FtpS::FtpS(const FtpS *o) : super(o)
{
   ftps=true;
   res_prefix="ftp";
   Reconfig(0);
}
FileAccess *FtpS::New(){ return new FtpS();}

void Ftp::Connection::MakeSSLBuffers(const char *hostname)
{
   Delete(control_send); control_send=0; telnet_layer_send=0;
   Delete(control_recv); control_recv=0;

   control_ssl=lftp_ssl_new(control_sock,hostname);
   IOBufferSSL *send_ssl=new IOBufferSSL(control_ssl,IOBufferSSL::PUT,hostname);
   IOBufferSSL *recv_ssl=new IOBufferSSL(control_ssl,IOBufferSSL::GET,hostname);
   send_ssl->DoConnect();
   recv_ssl->CloseLater();

   control_send=send_ssl;
   control_recv=recv_ssl;
}
#endif

void IOBufferTelnet::PutTranslated(const char *put_buf,int size)
{
   bool from_untranslated=false;
   if(untranslated && untranslated->Size()>0)
   {
      untranslated->Put(put_buf,size);
      untranslated->Get(&put_buf,&size);
      from_untranslated=true;
   }
   if(size<=0)
      return;
   size_t put_size=size;
   const char *iac;
   while(put_size>0)
   {
      iac=(const char*)memchr(put_buf,TELNET_IAC,put_size);
      if(!iac)
	 break;
      Buffer::Put(put_buf,iac-put_buf);
      if(from_untranslated)
	 untranslated->Skip(iac-put_buf);
      put_size-=iac-put_buf;
      put_buf=iac;
      if(mode==PUT)
      {
	 // double the IAC to send it literally.
	 Buffer::Put(iac,1);
	 Buffer::Put(iac,1);
	 if(from_untranslated)
	    untranslated->Skip(1);
	 put_buf++;
	 put_size--;
      }
      else // mode==GET
      {
	 if(put_size<2)
	 {
	    if(!from_untranslated)
	    {
	       if(!untranslated)
		  untranslated=new Buffer;
	       untranslated->Put(iac,1);
	    }
	    return;
	 }
	 switch((unsigned char)iac[1])
	 {
	 case TELNET_IAC:
	    Buffer::Put(iac,1);
	    /*fallthrough*/
	 default:
	    if(from_untranslated)
	       untranslated->Skip(2);
	    put_buf+=2;
	    put_size-=2;
	 }
      }
   }
   if(put_size>0)
      Buffer::Put(put_buf,put_size);
}

void Ftp::Connection::SetControlConnectionTranslation(const char *cs)
{
   if(translation_activated)
      return;
   if(telnet_layer_send==control_send)
   {
      // cannot do two conversions in one DirectedBuffer, stack it.
      control_send=new IOBufferStacked(control_send);
      control_recv=new IOBufferStacked(control_recv);
   }
   control_send->SetTranslation(cs,false);
   control_recv->SetTranslation(cs,true);
   translation_activated=true;
}

#include "modconfig.h"
#ifdef MODULE_PROTO_FTP
void module_init()
{
   Ftp::ClassInit();
}
#endif

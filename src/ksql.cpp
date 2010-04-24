/*
 * ksql.cpp
 *
 *  Created on: 2009-10-21
 *      Author: cai
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef DEBUG
#include <iostream>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <poll.h>
#include <net/ethernet.h>
#include <glib.h>
#include "./KSQL/kmysql.h"
#include "ksql_old.h"


static const char	SQL_template[]=
	"insert into t_netlog (RoomNum,MachineIP,MachineMac,CustomerIDType,CustomerIDNum, "
		"CustomerName,nLogType,strLogInfo,nTime) values   ('%s%s%02d','%s','%s','%s','%s','%s','%s','%s','%s')";


static FUNC_SENDDATA SendData = NOP_SENDDATA;
int kregisterSendDataFunc(FUNC_SENDDATA f){	SendData = f;return 0;}

void formattime(GString * strtime, struct tm* pTm)
{

	g_string_printf(strtime,"%d-%d-%d %d:%d:%d",
            pTm->tm_year+1900,pTm->tm_mon+1,pTm->tm_mday,
            pTm->tm_hour,pTm->tm_min,pTm->tm_sec);
}

void formattime(GString * strtime)
{
	struct tm pTm;
	time_t t = time(0);
	pTm = *localtime(&t);
	formattime(strtime,&pTm);
}

static void MAC_ADDR2macaddr(char mac_addr[PROLEN_COMPUTERMAC],const u_char mac[ETHER_ADDR_LEN])
{
	sprintf(mac_addr,"%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static volatile int	ksql_inited=false;
static volatile int	ksql_usemysql=false;

void ksql_query_and_use_result(void(*callback)(KSQL_ROW row, void*p),
		const char* query, void*p)
{
	KSQL_RES * res;
	KSQL_ROW	row;
	res = ksql_query_and_use_result(query); //内部都开始使用自己封装的函数了。呵呵
	while ((row = ksql_fetch_row(res)))
	{
		if (callback) //避免错误啊，呵呵
			callback(row, p);
	}
	ksql_free_result(res);
}

void InsertCustomerLog(const char * build,const char * floor,const char * room, const char * name ,
		const char * idtype , const char * id, const char * type,const char * ip, const char * mac, const char * time)
{
	GString *  sqlstr = g_string_new("");

	g_string_printf(sqlstr,"insert into t_customerlog ("
			"BuildNum,RoomFloor,RoomNum,CustomerName,CustomerIDType,"
			"CustomerIDNum,MachineIP,MachineMac,nType,HappyTime) "
			"values ('%s','%s','%s','%s','%s','%s','%s','%s','%s','%s')",
			build,floor,room,name,idtype,id,ip,mac,type,time);
	//log_printf(L_DEBUG_OUTPUT,"%s\n",sqlstr.c_str());
	ksql_run_query_async(sqlstr->str);
	g_string_free(sqlstr,TRUE);
}

KSQL_ROW ksql_fetch_row(KSQL_RES*res)
{
	if (res)
	{
		if (ksql_usemysql)
			return kmysql_fetch_row(res);
		else
			return ksqlite_fetch_row(res);
	}
	return NULL;
}

static int	ksql_daemon_socket=0;
static void * KSQL_daemon(void*_p)
{
	int ksql_daemon_socket_recv ;
	pollfd	pfd;

	char	buf[4096];

	struct
	{
		pthread_mutex_t p;
		int fd[2];
	} *p = (typeof(p))_p;

	ksql_daemon_socket_recv = p->fd[0];
	shutdown(ksql_daemon_socket_recv,1);

	pthread_mutex_unlock(&p->p);

	KSQL_RES * res;
	KSQL_ROW	row;

	const gchar * pswd, *user, *host, *database;

	extern GKeyFile * gkeyfile;

	pswd = g_key_file_get_string(gkeyfile,"monitor","db.config.password",NULL);
	if(!pswd) pswd = "";
	user = g_key_file_get_string(gkeyfile,"monitor","db.config.username",NULL);
	if(!user) user = "root";

	host = g_key_file_get_string(gkeyfile,"monitor","db.config.host", NULL);
	if(!host) host = "localhost";

	database = g_key_file_get_string(gkeyfile,"monitor","db.config.dbname", NULL);
	if(!host)
		host = "hotel";

#ifdef WITH_SQLITE3
	//初始化 SQLite 数据库。不可以失败!
	InitSqlite();
#endif

	for (;;)
	{
		ksql_inited = 0;

#ifdef WITH_SQLITE3
		if (InitMySQL(pswd, user, database, host))
		{
			syslog(LOG_ERR, "Cannot connect to MYSQL server, use SQLite !");
		}else
			ksql_usemysql = true;

#else
		while (InitMySQL(pswd, user, database, host))
		{
			syslog(LOG_ERR, "Cannot connect to MYSQL server, sleep and retry!");
			sleep(2);
		}
		ksql_usemysql = true;
#endif

		//Get t_sysparam
		res = (typeof(res)) ksql_query_and_use_result(
				"select * from t_sysparam");

		row = ksql_fetch_row(res);
		if (row)
		{
			strcpy(hotel::strServerIP, row[1]);
			strcpy(hotel::strHotelID, row[2]);
			strcpy(hotel::str_ethID + 3, row[3]);

			utf8_gbk(hotel::strHoteName, sizeof(hotel::strHoteName), row[4],
					strlen(row[4]));

#ifdef ENABLE_HOTEL
			strcpy(hotel::strWebIP, row[5]);
#endif

			syslog(LOG_NOTICE, "hotel name is %s\n", row[4]);
			syslog(LOG_NOTICE, "hotel ID is %s\n", hotel::strHotelID);
			syslog(LOG_NOTICE, "ServerIP is %s\n", hotel::strServerIP);
#ifdef ENABLE_HOTEL
			syslog(LOG_NOTICE,"WebIP is %s\n",hotel::strWebIP);
#endif
		}
		else if (!res)
		{
			syslog(LOG_WARNING, "tables not exist, create them.\n");
			if(ksql_usemysql)
				InitMysqlDB();
			InitSqliteDB();
			syslog(LOG_NEWS, "All tables created!");
			exit(0);
		}
		ksql_free_result(res);
#ifdef ENABLE_HOTEL
		//初始化跳转页面
		GString * dest = g_string_new("");
		g_string_printf(dest,"%s/login", hotel::strWebIP);

		init_http_redirector(dest->str);
		g_string_free(dest,1);
#endif

	//------------------------------------------
		//现在，开始检测是否断开了数据库的连接，呵呵，断开连接就重新连接上
		//并且为异步做服务，哈哈
		ksql_inited = 2;

		for(;;)
		{
			pfd.fd = ksql_daemon_socket_recv;

			pfd.events = POLLIN;

			while(poll(&pfd,1,10000))
			{
				//执行代码
				(void)read(ksql_daemon_socket_recv,buf,4096);
				printf("%s\n",buf);
				if (ksql_run_query(buf))
					break;
			}

			if (is_mysqlserver_gone())
			{
				kmysql_close();
				break;
			}else{
				//修复数据库，神州行，我看行。
				ksql_run_query("repair table t_netlog");
			}
		}
	}
	return 0;
}



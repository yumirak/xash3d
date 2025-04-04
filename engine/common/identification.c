/*
identification.c - unique id generation
Copyright (C) 2017 mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include <fcntl.h>
#ifndef _WIN32
#include <dirent.h>
#endif
static char id_md5[33];
static char id_customid[MAX_STRING];

/*
==========================================================

simple 64-bit one-hash-func bloom filter
should be enough to determine if device exist in identifier

==========================================================
*/
typedef uint64_t bloomfilter_t;

static bloomfilter_t id;

#define bf64_mask ((1U<<6)-1)

bloomfilter_t BloomFilter_Process( const char *buffer, int size )
{
	uint32_t crc32;
	bloomfilter_t value = 0;

	if( size <= 0 || size > 512 )
		return 0;

	CRC32_Init( &crc32 );
	CRC32_ProcessBuffer( &crc32, buffer, size );

	while( crc32 )
	{
		value |= ((bloomfilter_t)1) << ( crc32 & bf64_mask );
		crc32 = crc32 >> 6;
	}

	return value;
}

bloomfilter_t BloomFilter_ProcessStr( const char *buffer )
{
	return BloomFilter_Process( buffer, Q_strlen( buffer ) );
}

uint32_t BloomFilter_Weight( bloomfilter_t value )
{
	int weight = 0;

	while( value )
	{
		if( value & 1 )
			weight++;
		value = value >> 1;
	}

	return weight;
}

qboolean BloomFilter_ContainsString( bloomfilter_t filter, const char *str )
{
	bloomfilter_t value = BloomFilter_ProcessStr( str );

	return (filter & value) == value;
}

/*
=============================================

IDENTIFICATION

=============================================
*/
#define MAXBITS_GEN 30
#define MAXBITS_CHECK MAXBITS_GEN + 6

qboolean ID_ProcessFile( bloomfilter_t *value, const char *path );

void ID_BloomFilter_f( void )
{
	bloomfilter_t value = 0;
	int i;

	for( i = 1; i < Cmd_Argc(); i++ )
		value |= BloomFilter_ProcessStr( Cmd_Argv( i ) );

	Msg( "%d %016"PRIX64"\n", BloomFilter_Weight( value ), value );

	// test
	// for( i = 1; i < Cmd_Argc(); i++ )
	//	Msg( "%s: %d\n", Cmd_Argv( i ), BloomFilter_ContainsString( value, Cmd_Argv( i ) ) );
}

qboolean ID_Verify( const char *hex )
{
	uint32_t chars = 0;
	char prev = 0;
	qboolean monotonic = true; // detect 11:22...
	int weight = 0;
	size_t lenght = 0;

	while( *hex )
	{
		char ch = Q_tolower( *hex++ );

		if( ( ch >= 'a' && ch <= 'f') || ( ch >= '0' && ch <= '9' ) )
		{
			if( prev && ( ch - prev < -1 || ch - prev > 1 ) )
				monotonic = false;

			if( ch >= 'a' )
				chars |= 1 << (ch - 'a' + 10);
			else
				chars |= 1 << (ch - '0');

			prev = ch;
		} else
			return false;

		lenght++;
	}

	if( monotonic )
		return false;

	// md5 16 byte * 2 char/byte = 32 chars
	if( lenght != 32 )
		return false;

	while( chars )
	{
		if( chars & 1 )
			weight++;

		chars = chars >> 1;

		if( weight > 2 )
			return true;
	}

	return false;
}

qboolean ID_VerifyHEX( const char *hex )
{
	uint32_t chars = 0;
	int weight = 0;

	while( *hex )
	{
		char ch = Q_tolower( *hex++ );

		if( (ch >= 'a' && ch <= 'f') || ( ch >= '0' && ch <= '9' )  )
		{
			if( ch >= 'a' )
				chars |= 1 << (ch - 'a' + 10);
			else
				chars |= 1 << (ch - '0');
		} else
			return false;
	}

	while( chars )
	{
		if( chars & 1 )
			weight++;

		chars = chars >> 1;

		if( weight > 2 )
			return true;
	}

	return false;
}

void ID_VerifyHEX_f( void )
{
	if( ID_VerifyHEX( Cmd_Argv( 1 ) ) )
		Msg( "Good\n" );
	else
		Msg( "Bad\n" );
}

#ifdef __linux__

qboolean ID_ProcessCPUInfo( bloomfilter_t *value )
{
	int cpuinfofd = open( "/proc/cpuinfo", O_RDONLY );
	char buffer[1024], *pbuf, *pbuf2;
	int ret;

	if( cpuinfofd < 0 )
		return false;

	if( (ret = read( cpuinfofd, buffer, 1023 ) ) < 0 )
	{
		close( cpuinfofd );
		return false;
	}

	close( cpuinfofd );

	buffer[ret] = 0;

	if( !ret )
		return false;

	pbuf = Q_strstr( buffer, "Serial" );
	if( !pbuf )
		return false;
	pbuf += 6;

	if( ( pbuf2 = Q_strchr( pbuf, '\n' ) ) )
		*pbuf2 = 0;
	else
		pbuf2 = pbuf + Q_strlen( pbuf );

	if( !ID_VerifyHEX( pbuf ) )
		return false;

	*value |= BloomFilter_Process( pbuf, pbuf2 - pbuf );
	return true;
}

qboolean ID_ValidateNetDevice( const char *dev )
{
	const char *prefix = "/sys/class/net";
	byte *pfile;
	int assignType;

	// These devices are fake, their mac address is generated each boot, while assign_type is 0
	if( !Q_strnicmp( dev, "ccmni", sizeof( "ccmni" ) ) ||
		!Q_strnicmp( dev, "ifb", sizeof( "ifb" ) ) )
		return false;

	pfile = FS_LoadDirectFile( va( "%s/%s/addr_assign_type", prefix, dev ), NULL );

	// if NULL, it may be old kernel
	if( pfile )
	{
		assignType = Q_atoi( (char*)pfile );

		Mem_Free( pfile );

		// check is MAC address is constant
		if( assignType != 0 )
			return false;
	}

	return true;
}

int ID_ProcessNetDevices( bloomfilter_t *value )
{
	const char *prefix = "/sys/class/net";
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	if( !( dir = opendir( prefix ) ) )
		return 0;

	while( ( entry = readdir( dir ) ) && BloomFilter_Weight( *value ) < MAXBITS_GEN )
	{
		if( !Q_strcmp( entry->d_name, "." ) || !Q_strcmp( entry->d_name, ".." ) )
			continue;

		if( !ID_ValidateNetDevice( entry->d_name ) )
			continue;

		count += ID_ProcessFile( value, va( "%s/%s/address", prefix, entry->d_name ) );
	}
	closedir( dir );
	return count;
}

int ID_CheckNetDevices( bloomfilter_t value )
{
	const char *prefix = "/sys/class/net";

	DIR *dir;
	struct dirent *entry;
	int count = 0;
	bloomfilter_t filter = 0;

	if( !( dir = opendir( prefix ) ) )
		return 0;

	while( ( entry = readdir( dir ) ) )
	{
		if( !Q_strcmp( entry->d_name, "." ) || !Q_strcmp( entry->d_name, ".." ) )
			continue;

		if( !ID_ValidateNetDevice( entry->d_name ) )
			continue;

		if( ID_ProcessFile( &filter, va( "%s/%s/address", prefix, entry->d_name ) ) )
			count += ( value & filter ) == filter, filter = 0;
	}

	closedir( dir );
	return count;
}

void ID_TestCPUInfo_f( void )
{
	bloomfilter_t value = 0;

	if( ID_ProcessCPUInfo( &value ) )
		Msg( "Got %016"PRIX64"\n", value );
	else
		Msg( "Could not get serial\n" );
}

#endif

qboolean ID_ProcessFile( bloomfilter_t *value, const char *path )
{
	int fd = open( path, O_RDONLY );
	char buffer[256];
	int ret;

	if( fd < 0 )
		return false;

	if( (ret = read( fd, buffer, 255 ) ) < 0 )
	{
		close( fd );
		return false;
	}

	close( fd );

	if( !ret )
		return false;

	buffer[ret] = 0;

	if( !ID_VerifyHEX( buffer ) )
		return false;

	*value |= BloomFilter_Process( buffer, ret );
	return true;
}

#ifndef _WIN32
int ID_ProcessFiles( bloomfilter_t *value, const char *prefix, const char *postfix )
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	if( !( dir = opendir( prefix ) ) )
	    return 0;

	while( ( entry = readdir( dir ) ) && BloomFilter_Weight( *value ) < MAXBITS_GEN )
	{
		if( !Q_strcmp( entry->d_name, "." ) || !Q_strcmp( entry->d_name, ".." ) )
			continue;

		count += ID_ProcessFile( value, va( "%s/%s/%s", prefix, entry->d_name, postfix ) );
	}
	closedir( dir );
	return count;
}

int ID_CheckFiles( bloomfilter_t value, const char *prefix, const char *postfix )
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;
	bloomfilter_t filter = 0;

	if( !( dir = opendir( prefix ) ) )
	    return 0;

	while( ( entry = readdir( dir ) ) )
	{
		if( !Q_strcmp( entry->d_name, "." ) || !Q_strcmp( entry->d_name, ".." ) )
			continue;

		if( ID_ProcessFile( &filter, va( "%s/%s/%s", prefix, entry->d_name, postfix ) ) )
			count += ( value & filter ) == filter, filter = 0;
	}

	closedir( dir );
	return count;
}
#else
int ID_GetKeyData( HKEY hRootKey, char *subKey, char *value, LPBYTE data, DWORD cbData )
{
	HKEY hKey;

	if( RegOpenKeyEx( hRootKey, subKey, 0, KEY_QUERY_VALUE, &hKey ) != ERROR_SUCCESS )
		return 0;
	
	if( RegQueryValueEx( hKey, value, NULL, NULL, data, &cbData ) != ERROR_SUCCESS )
	{
		RegCloseKey( hKey );
		return 0;
	}

	RegCloseKey( hKey );
	return 1;
}
int ID_SetKeyData( HKEY hRootKey, char *subKey, DWORD dwType, char *value, LPBYTE data, DWORD cbData)
{
	HKEY hKey;
	if( RegCreateKey( hRootKey, subKey, &hKey ) != ERROR_SUCCESS )
		return 0;
	
	if( RegSetValueEx( hKey, value, 0, dwType, data, cbData ) != ERROR_SUCCESS )
	{
		RegCloseKey( hKey );
		return 0;
	}
	
	RegCloseKey( hKey );
	return 1;
}

#define BUFSIZE 4096

int ID_RunWMIC(char *buffer, const char *cmdline)
{
	HANDLE g_IN_Rd = NULL;
	HANDLE g_IN_Wr = NULL;
	HANDLE g_OUT_Rd = NULL;
	HANDLE g_OUT_Wr = NULL;
	DWORD dwRead;
	BOOL bSuccess = FALSE;
	SECURITY_ATTRIBUTES saAttr;
	
	STARTUPINFO si = {0};
	
	PROCESS_INFORMATION pi = {0};
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;
	
	CreatePipe( &g_IN_Rd, &g_IN_Wr, &saAttr, 0 );
	CreatePipe( &g_OUT_Rd, &g_OUT_Wr, &saAttr, 0 );
	SetHandleInformation( g_IN_Wr, HANDLE_FLAG_INHERIT, 0 );
	
	si.cb = sizeof(STARTUPINFO);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = g_IN_Rd;
	si.hStdOutput = g_OUT_Wr;
	si.hStdError = g_OUT_Wr;
	si.wShowWindow = SW_HIDE;
	si.dwFlags |= STARTF_USESTDHANDLES;

	CreateProcess( NULL, (char*)cmdline, NULL, NULL, true, CREATE_NO_WINDOW , NULL, NULL, &si, &pi );
	
	CloseHandle( g_OUT_Wr );
	CloseHandle( g_IN_Wr );
	
	WaitForSingleObject( pi.hProcess, 500 );
	
	bSuccess = ReadFile( g_OUT_Rd, buffer, BUFSIZE, &dwRead, NULL );
	buffer[BUFSIZE-1] = 0;
	CloseHandle( g_IN_Rd );
	CloseHandle( g_OUT_Rd );

	return bSuccess;
}

int ID_ProcessWMIC( bloomfilter_t *value, const char *cmdline )
{
	char buffer[BUFSIZE], token[BUFSIZE], *pbuf;
	int count = 0;

	if( !ID_RunWMIC( buffer, cmdline ) )
		return 0;
	pbuf = COM_ParseFile( buffer, token ); // Header
	while( pbuf = COM_ParseFile( pbuf, token ) )
	{
		if( !ID_VerifyHEX( token ) )
			continue;

		*value |= BloomFilter_ProcessStr( token );
		count ++;
	}

	return count;
}

int ID_CheckWMIC( bloomfilter_t value, const char *cmdline )
{
	char buffer[BUFSIZE], token[BUFSIZE], *pbuf;
	int count = 0;

	if( !ID_RunWMIC( buffer, cmdline ) )
		return 0;
	pbuf = COM_ParseFile( buffer, token ); // Header
	while( pbuf = COM_ParseFile( pbuf, token ) )
	{
		bloomfilter_t filter;

		if( !ID_VerifyHEX( token ) )
			continue;

		filter = BloomFilter_ProcessStr( token );
		count += ( filter & value ) == filter;
	}

	return count;
}
#endif

bloomfilter_t ID_GenerateRawId( void )
{
	bloomfilter_t value = 0;
	int count = 0;

#ifdef __linux__
#if defined(__ANDROID__) && !defined(XASH_DEDICATED)
	{
		const char *androidid = Android_GetAndroidID();
		if( androidid && ID_VerifyHEX( androidid ) )
		{
			value |= BloomFilter_ProcessStr( androidid );
			count ++;
		}
	}
#endif
	count += ID_ProcessCPUInfo( &value );
	count += ID_ProcessFiles( &value, "/sys/block", "device/cid" );
	count += ID_ProcessNetDevices( &value );
#endif
#ifdef _WIN32
	count += ID_ProcessWMIC( &value, "wmic path win32_physicalmedia get SerialNumber " );
	count += ID_ProcessWMIC( &value, "wmic bios get serialnumber " );
#endif

	return value;
}

uint32_t ID_CheckRawId( bloomfilter_t filter )
{
	bloomfilter_t value = 0;
	int count = 0;

#ifdef __linux__
#if defined(__ANDROID__) && !defined(XASH_DEDICATED)
	{
		const char *androidid = Android_GetAndroidID();
		if( androidid && ID_VerifyHEX( androidid ) )
		{
			value = BloomFilter_ProcessStr( androidid );
			count += (filter & value) == value;
			value = 0;
		}
	}
#endif
	count += ID_CheckNetDevices( filter );
	count += ID_CheckFiles( filter, "/sys/block", "device/cid" );
	if( ID_ProcessCPUInfo( &value ) )
		count += (filter & value) == value;
#endif
	
#ifdef _WIN32
	count += ID_CheckWMIC( filter, "wmic path win32_physicalmedia get SerialNumber" );
	count += ID_CheckWMIC( filter, "wmic bios get serialnumber" );
#endif

#if 0
	Msg( "ID_CheckRawId: %d\n", count );
#endif
	return count;
}

#define SYSTEM_XOR_MASK 0x10331c2dce4c91db
#define GAME_XOR_MASK 0x7ffc48fbac1711f1

void ID_Check()
{
	uint32_t weight = BloomFilter_Weight( id );
	uint32_t mincount = weight >> 2;

	if( mincount < 1 )
		mincount = 1;

	if( weight > MAXBITS_CHECK )
	{
		id = 0;
#if 0
		Msg( "ID_Check(): fail %d\n", weight );
#endif
		return;
	}

	if( ID_CheckRawId( id ) < mincount )
		id = 0;
#if 0
	Msg( "ID_Check(): success %d\n", weight );
#endif
}

const char *ID_GetMD5()
{
	// tyabus: Used by some malicious actors, disabling.
	/*if ( id_customid[0] )
		return id_customid;*/
	return id_md5;
}

/*
===============
ID_SetCustomClientID

===============
*/
void GAME_EXPORT ID_SetCustomClientID( const char *id )
{
	if( !id )
		return;

	Q_strncpy( id_customid, id, sizeof( id_customid  ) );
}

void ID_Init( void )
{
	MD5Context_t hash = {0};
	byte md5[16];
	int i;

	Cmd_AddCommand( "bloomfilter", ID_BloomFilter_f, "print bloomfilter raw value of arguments set");
	Cmd_AddCommand( "verifyhex", ID_VerifyHEX_f, "check if id source seems to be fake" );
#ifdef __linux__
	Cmd_AddCommand( "testcpuinfo", ID_TestCPUInfo_f, "try read cpu serial" );
#endif

#if defined(__ANDROID__) && !defined(XASH_DEDICATED)
	sscanf( Android_LoadID(), "%016"PRIX64, &id );
	if( id )
	{
		id ^= SYSTEM_XOR_MASK;
		ID_Check();
	}
	
#elif defined _WIN32
	{
		CHAR szBuf[MAX_PATH];
		ID_GetKeyData( HKEY_CURRENT_USER, "Software\\Xash3D\\", "xash_id", szBuf, MAX_PATH );
		
		sscanf(szBuf, "%016"PRIX64, &id);
		id ^= SYSTEM_XOR_MASK;
		ID_Check();
	}
#else
	{
#ifndef __HAIKU__
		const char *home = getenv( "HOME" );
#else
		char home[MAX_SYSPATH];
		find_directory( B_USER_SETTINGS_DIRECTORY, -1, false, home, MAX_SYSPATH );
#endif
		if( home )
		{
			FILE *cfg = fopen( va( "%s/.config/.xash_id", home ), "r" );
			if( !cfg )
				cfg = fopen( va( "%s/.local/.xash_id", home ), "r" );
			if( !cfg )
				cfg = fopen( va( "%s/.xash_id", home ), "r" );
			if( cfg )
			{
				if( fscanf( cfg, "%016"PRIX64, &id ) > 0 )
				{
					id ^= SYSTEM_XOR_MASK;
					ID_Check();
				}
				fclose( cfg );
			}
		}
	}
#endif
	if( !id )
	{
		const char *buf = (const char*) FS_LoadFile( ".xash_id", NULL, false );
		if( buf )
		{
			sscanf( buf, "%016"PRIX64, &id );
			id ^= GAME_XOR_MASK;
			ID_Check();
			Mem_Free( (void*)buf );
		}
	}
	if( !id )
		id = ID_GenerateRawId();

	MD5Init( &hash );
	MD5Update( &hash, (byte *)&id, sizeof( id ) );
	MD5Final( (byte*)md5, &hash );

	for( i = 0; i < 16; i++ )
		Q_sprintf( &id_md5[i*2], "%02hhx", md5[i] );

#if defined(__ANDROID__) && !defined(XASH_DEDICATED)
	Android_SaveID( va("%016"PRIX64, id^SYSTEM_XOR_MASK ) );
#elif defined _WIN32
	{
		CHAR Buf[MAX_PATH];
		sprintf( Buf, "%016"PRIX64, id^SYSTEM_XOR_MASK );
		ID_SetKeyData( HKEY_CURRENT_USER, "Software\\Xash3D\\", REG_SZ, "xash_id", Buf, Q_strlen(Buf) );
	}
#else
	{
#ifndef __HAIKU__
		const char *home = getenv( "HOME" );
#else
		char home[MAX_SYSPATH];
		find_directory( B_USER_SETTINGS_DIRECTORY, -1, false, home, MAX_SYSPATH );
#endif
		if( home )
		{
			FILE *cfg = fopen( va( "%s/.config/.xash_id", home ), "w" );
			if( !cfg )
				cfg = fopen( va( "%s/.local/.xash_id", home ), "w" );
			if( !cfg )
				cfg = fopen( va( "%s/.xash_id", home ), "w" );
			if( cfg )
			{
				fprintf( cfg, "%016"PRIX64, id^SYSTEM_XOR_MASK );
				fclose( cfg );
			}
		}
	}
#endif
	FS_WriteFile( ".xash_id", va("%016"PRIX64, id^GAME_XOR_MASK), 16 );
#if 0
	Msg("MD5 id: %s\nRAW id:%016"PRIX64"\n", id_md5, id );
#endif
}

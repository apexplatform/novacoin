// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "util.h"
#include "sync.h"
#include "version.h"
#include "ui_interface.h"

#include <random>

#include <boost/program_options/detail/config_file.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#ifdef WIN32
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501
#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h> /* for _commit */
#include "shlobj.h"
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

#if !defined(WIN32) && !defined(ANDROID)
#include <execinfo.h>
#endif

using namespace std;
namespace bt = boost::posix_time;

map<string, string> mapArgs;
map<string, vector<string> > mapMultiArgs;
bool fDebug = false;
bool fDebugNet = false;
bool fPrintToConsole = false;
bool fPrintToDebugger = false;
bool fRequestShutdown = false;
bool fShutdown = false;
bool fDaemon = false;
bool fServer = false;
bool fCommandLine = false;
string strMiscWarning;
bool fTestNet = false;
bool fNoListen = false;
bool fLogTimestamps = false;
CMedianFilter<int64_t> vTimeOffsets(200,0);
bool fReopenDebugLog = false;

// Extended DecodeDumpTime implementation, see this page for details:
// http://stackoverflow.com/questions/3786201/parsing-of-date-time-from-string-boost
const locale formats[] = {
    locale(locale::classic(),new bt::time_input_facet("%Y-%m-%dT%H:%M:%SZ")),
    locale(locale::classic(),new bt::time_input_facet("%Y-%m-%d %H:%M:%S")),
    locale(locale::classic(),new bt::time_input_facet("%Y/%m/%d %H:%M:%S")),
    locale(locale::classic(),new bt::time_input_facet("%d.%m.%Y %H:%M:%S")),
    locale(locale::classic(),new bt::time_input_facet("%Y-%m-%d"))
};

const size_t formats_n = sizeof(formats)/sizeof(formats[0]);

time_t pt_to_time_t(const bt::ptime& pt)
{
    bt::ptime timet_start(boost::gregorian::date(1970,1,1));
    bt::time_duration diff = pt - timet_start;
    return diff.ticks()/bt::time_duration::rep_type::ticks_per_second;
}

// Init OpenSSL library multithreading support
static CCriticalSection** ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line)
{
    if (mode & CRYPTO_LOCK) {
        ENTER_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    } else {
        LEAVE_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    }
}

LockedPageManager LockedPageManager::instance;

// Init
class CInit
{
public:
    CInit()
    {
        // Init OpenSSL library multithreading support
        ppmutexOpenSSL = (CCriticalSection**)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(CCriticalSection*));
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            ppmutexOpenSSL[i] = new CCriticalSection();
        CRYPTO_set_locking_callback(locking_callback);

#ifdef WIN32
        // Seed random number generator with screen scrape and other hardware sources
        RAND_screen();
#endif

        // Seed random number generator with performance counter
        RandAddSeed();
    }
    ~CInit()
    {
        // Shutdown OpenSSL library multithreading support
        CRYPTO_set_locking_callback(NULL);
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            delete ppmutexOpenSSL[i];
        OPENSSL_free(ppmutexOpenSSL);
    }
}
instance_of_cinit;

void RandAddSeed()
{
    // Seed with CPU performance counter
    int64_t nCounter = GetPerformanceCounter();
    RAND_add(&nCounter, sizeof(nCounter), 1.5);
    memset(&nCounter, 0, sizeof(nCounter));
}

void RandAddSeedPerfmon()
{
    RandAddSeed();

    // This can take up to 2 seconds, so only do it every 10 minutes
    static int64_t nLastPerfmon;
    if (GetTime() < nLastPerfmon + 10 * 60)
        return;
    nLastPerfmon = GetTime();

#ifdef WIN32
    // Don't need this on Linux, OpenSSL automatically uses /dev/urandom
    // Seed with the entire set of perfmon data
    unsigned char pdata[250000];
    memset(pdata, 0, sizeof(pdata));
    unsigned long nSize = sizeof(pdata);
    long ret = RegQueryValueExA(HKEY_PERFORMANCE_DATA, "Global", NULL, NULL, pdata, &nSize);
    RegCloseKey(HKEY_PERFORMANCE_DATA);
    if (ret == ERROR_SUCCESS)
    {
        RAND_add(pdata, nSize, nSize/100.0);
        OPENSSL_cleanse(pdata, nSize);
        printf("RandAddSeed() %lu bytes\n", nSize);
    }
#endif
}

uint64_t GetRand(uint64_t nMax)
{
    if (nMax == 0)
        return 0;

    // The range of the random source must be a multiple of the modulus
    // to give every possible output value an equal possibility
    uint64_t nRange = (numeric_limits<uint64_t>::max() / nMax) * nMax;
    uint64_t nRand = 0;
    do
        RAND_bytes((unsigned char*)&nRand, sizeof(nRand));
    while (nRand >= nRange);
    return (nRand % nMax);
}

int GetRandInt(int nMax)
{
    return static_cast<int>(GetRand(nMax));
}

uint256 GetRandHash()
{
    uint256 hash;
    RAND_bytes(hash.begin(), hash.size());
    return hash;
}

void FillRand(uint8_t *buffer, size_t nCount)
{
    RAND_bytes(buffer, nCount);
}

static FILE* fileout = NULL;

inline int OutputDebugStringF(const char* pszFormat, ...)
{
    int ret = 0;
    if (fPrintToConsole)
    {
        // print to console
        va_list arg_ptr;
        va_start(arg_ptr, pszFormat);
        ret = vprintf(pszFormat, arg_ptr);
        va_end(arg_ptr);
    }
    else if (!fPrintToDebugger)
    {
        // print to debug.log

        if (!fileout)
        {
            boost::filesystem::path pathDebug = GetDataDir() / "debug.log";
            fileout = fopen(pathDebug.string().c_str(), "a");
            if (fileout) setbuf(fileout, NULL); // unbuffered
        }
        if (fileout)
        {
            static bool fStartedNewLine = true;

            // This routine may be called by global destructors during shutdown.
            // Since the order of destruction of static/global objects is undefined,
            // allocate mutexDebugLog on the heap the first time this routine
            // is called to avoid crashes during shutdown.
            static boost::mutex* mutexDebugLog = NULL;
            if (mutexDebugLog == NULL) mutexDebugLog = new boost::mutex();
            boost::mutex::scoped_lock scoped_lock(*mutexDebugLog);

            // reopen the log file, if requested
            if (fReopenDebugLog) {
                fReopenDebugLog = false;
                boost::filesystem::path pathDebug = GetDataDir() / "debug.log";
                if (freopen(pathDebug.string().c_str(),"a",fileout) != NULL)
                    setbuf(fileout, NULL); // unbuffered
            }

            // Debug print useful for profiling
            if (fLogTimestamps && fStartedNewLine)
                fprintf(fileout, "%s ", DateTimeStrFormat("%x %H:%M:%S", GetTime()).c_str());
            if (pszFormat[strlen(pszFormat) - 1] == '\n')
                fStartedNewLine = true;
            else
                fStartedNewLine = false;

            va_list arg_ptr;
            va_start(arg_ptr, pszFormat);
            ret = vfprintf(fileout, pszFormat, arg_ptr);
            va_end(arg_ptr);
        }
    }

#ifdef WIN32
    if (fPrintToDebugger)
    {
        static CCriticalSection cs_OutputDebugStringF;

        // accumulate and output a line at a time
        {
            LOCK(cs_OutputDebugStringF);
            static string buffer;

            va_list arg_ptr;
            va_start(arg_ptr, pszFormat);
            buffer += vstrprintf(pszFormat, arg_ptr);
            va_end(arg_ptr);

            size_t line_start = 0, line_end;
            while((line_end = buffer.find('\n', line_start)) != string::npos)
            {
                OutputDebugStringA(buffer.substr(line_start, line_end - line_start).c_str());
                line_start = line_end + 1;
            }
            buffer.erase(0, line_start);
        }
    }
#endif
    return ret;
}

string vstrprintf(const char *format, va_list ap)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    for ( ; ; )
    {
#ifndef _MSC_VER
        va_list arg_ptr;
        va_copy(arg_ptr, ap);
#else
        va_list arg_ptr = ap;
#endif
#ifdef WIN32
        ret = _vsnprintf(p, limit, format, arg_ptr);
#else
        ret = vsnprintf(p, limit, format, arg_ptr);
#endif
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new(nothrow) char[limit];
        if (p == NULL)
            throw bad_alloc();
    }
    string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}

string real_strprintf(const char *format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    string str = vstrprintf(format, arg_ptr);
    va_end(arg_ptr);
    return str;
}

string real_strprintf(const string &format, int dummy, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, dummy);
    string str = vstrprintf(format.c_str(), arg_ptr);
    va_end(arg_ptr);
    return str;
}

bool error(const char *format, ...)
{
    va_list arg_ptr;
    va_start(arg_ptr, format);
    string str = vstrprintf(format, arg_ptr);
    va_end(arg_ptr);
    printf("ERROR: %s\n", str.c_str());
    return false;
}


void ParseString(const string& str, char c, vector<string>& v)
{
    if (str.empty())
        return;
    string::size_type i1 = 0;
    string::size_type i2;
    for ( ; ; )
    {
        i2 = str.find(c, i1);
        if (i2 == str.npos)
        {
            v.push_back(str.substr(i1));
            return;
        }
        v.push_back(str.substr(i1, i2-i1));
        i1 = i2+1;
    }
}

string FormatMoney(int64_t n, bool fPlus)
{
    ostringstream ss;

    if (n < 0)
        ss << '-';
    else if (fPlus && n > 0)
        ss << '+';
    ss << abs(n/COIN) << '.' << setfill('0') << setw(6) << abs(n%COIN);
    string str = ss.str();
    // Right-trim excess zeros before the decimal point:
    size_t nTrim = 0;
    for (size_t i = str.size()-1; (str[i] == '0' && isdigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    return str;
}

bool ParseMoney(const string& str, int64_t& nRet)
{
    return ParseMoney(str.c_str(), nRet);
}

bool ParseMoney(const char* pszIn, int64_t& nRet)
{
    string strWhole;
    int64_t nUnits = 0;
    const char* p = pszIn;
    while (isspace(*p))
        p++;
    for (; *p; p++)
    {
        if (*p == '.')
        {
            p++;
            int64_t nMult = CENT*10;
            while (isdigit(*p) && (nMult > 0))
            {
                nUnits += nMult * (*p++ - '0');
                nMult /= 10;
            }
            break;
        }
        if (isspace(*p))
            break;
        if (!isdigit(*p))
            return false;
        strWhole.insert(strWhole.end(), *p);
    }
    for (; *p; p++)
        if (!isspace(*p))
            return false;
    if (strWhole.size() > 10) // guard against 63 bit overflow
        return false;
    if (nUnits < 0 || nUnits > COIN)
        return false;
    int64_t nWhole = strtoll(strWhole);
    int64_t nValue = nWhole*COIN + nUnits;

    nRet = nValue;
    return true;
}

static const int8_t phexdigit[256] =
{
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     0,  1,  2,  3,  4,  5,  6, 7, 8, 9,-1,-1,-1,-1,-1,-1,
    -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, -1, -1, -1, -1, -1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

bool IsHex(const string& str)
{
    for(unsigned char c :  str)
    {
        if (phexdigit[c] < 0)
            return false;
    }
    return (str.size() > 0) && (str.size()%2 == 0);
}

vector<unsigned char> ParseHex(const char* psz)
{
    // convert hex dump to vector
    vector<unsigned char> vch;
    for ( ; ; )
    {
        while (isspace(*psz))
            psz++;
        auto c = phexdigit[(unsigned char)*psz++];
        if (c == (signed char)-1)
            break;
        auto n = (c << 4);
        c = phexdigit[(uint8_t)*psz++];
        if (c == (int8_t)-1)
            break;
        n |= c;
        vch.push_back(n);
    }
    return vch;
}

static void InterpretNegativeSetting(string name, map<string, string>& mapSettingsRet)
{
    // interpret -nofoo as -foo=0 (and -nofoo=0 as -foo=1) as long as -foo not set
    if (name.find("-no") == 0)
    {
        string positive("-");
        positive.append(name.begin()+3, name.end());
        if (mapSettingsRet.count(positive) == 0)
        {
            bool value = !GetBoolArg(name);
            mapSettingsRet[positive] = (value ? "1" : "0");
        }
    }
}

void ParseParameters(int argc, const char* const argv[])
{
    mapArgs.clear();
    mapMultiArgs.clear();
    for (int i = 1; i < argc; i++)
    {
        string str(argv[i]);
        string strValue;
        size_t is_index = str.find('=');
        if (is_index != string::npos)
        {
            strValue = str.substr(is_index+1);
            str = str.substr(0, is_index);
        }
#ifdef WIN32
        transform(str.begin(), str.end(), str.begin(), ::tolower);
        if (str.compare(0,1, "/") == 0)
            str = "-" + str.substr(1);
#endif
        if (str[0] != '-')
            break;

        mapArgs[str] = strValue;
        mapMultiArgs[str].push_back(strValue);
    }

    // New 0.6 features:
    for(const auto& entry : mapArgs)
    {
        string name = entry.first;

        //  interpret --foo as -foo (as long as both are not set)
        if (name.find("--") == 0)
        {
            string singleDash(name.begin()+1, name.end());
            if (mapArgs.count(singleDash) == 0)
                mapArgs[singleDash] = entry.second;
            name = singleDash;
        }

        // interpret -nofoo as -foo=0 (and -nofoo=0 as -foo=1) as long as -foo not set
        InterpretNegativeSetting(name, mapArgs);
    }
}

string GetArg(const string& strArg, const string& strDefault)
{
    if (mapArgs.count(strArg))
        return mapArgs[strArg];
    return strDefault;
}

int64_t GetArg(const string& strArg, int64_t nDefault)
{
    if (mapArgs.count(strArg))
        return strtoll(mapArgs[strArg]);
    return nDefault;
}

int32_t GetArgInt(const string& strArg, int32_t nDefault)
{
    if (mapArgs.count(strArg))
        return strtol(mapArgs[strArg]);
    return nDefault;
}

uint32_t GetArgUInt(const string& strArg, uint32_t nDefault)
{
    if (mapArgs.count(strArg))
        return strtoul(mapArgs[strArg]);
    return nDefault;
}

bool GetBoolArg(const string& strArg, bool fDefault)
{
    if (mapArgs.count(strArg))
    {
        if (mapArgs[strArg].empty())
            return true;
        return (atoi(mapArgs[strArg]) != 0);
    }
    return fDefault;
}

bool SoftSetArg(const string& strArg, const string& strValue)
{
    if (mapArgs.count(strArg) || mapMultiArgs.count(strArg))
        return false;
    mapArgs[strArg] = strValue;
    mapMultiArgs[strArg].push_back(strValue);

    return true;
}

bool SoftSetBoolArg(const string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, string("1"));
    else
        return SoftSetArg(strArg, string("0"));
}


string EncodeBase64(const unsigned char* pch, size_t len)
{
    static const char *pbase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    string strRet="";
    strRet.reserve((len+2)/3*4);

    int mode=0, left=0;
    const unsigned char *pchEnd = pch+len;

    while (pch<pchEnd)
    {
        int enc = *(pch++);
        switch (mode)
        {
            case 0: // we have no bits
                strRet += pbase64[enc >> 2];
                left = (enc & 3) << 4;
                mode = 1;
                break;

            case 1: // we have two bits
                strRet += pbase64[left | (enc >> 4)];
                left = (enc & 15) << 2;
                mode = 2;
                break;

            case 2: // we have four bits
                strRet += pbase64[left | (enc >> 6)];
                strRet += pbase64[enc & 63];
                mode = 0;
                break;
        }
    }

    if (mode)
    {
        strRet += pbase64[left];
        strRet += '=';
        if (mode == 1)
            strRet += '=';
    }

    return strRet;
}

string EncodeBase64(const string& str)
{
    return EncodeBase64((const unsigned char*)str.c_str(), str.size());
}

vector<unsigned char> DecodeBase64(const char* p, bool* pfInvalid)
{
    static const int decode64_table[256] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1,
        -1, -1, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
        29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
        49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    if (pfInvalid)
        *pfInvalid = false;

    vector<unsigned char> vchRet;
    vchRet.reserve(strlen(p)*3/4);

    int mode = 0;
    int left = 0;

    for ( ; ; )
    {
         int dec = decode64_table[(unsigned char)*p];
         if (dec == -1) break;
         p++;
         switch (mode)
         {
             case 0: // we have no bits and get 6
                 left = dec;
                 mode = 1;
                 break;

              case 1: // we have 6 bits and keep 4
                  vchRet.push_back((left<<2) | (dec>>4));
                  left = dec & 15;
                  mode = 2;
                  break;

             case 2: // we have 4 bits and get 6, we keep 2
                 vchRet.push_back((left<<4) | (dec>>2));
                 left = dec & 3;
                 mode = 3;
                 break;

             case 3: // we have 2 bits and get 6
                 vchRet.push_back((left<<6) | dec);
                 mode = 0;
                 break;
         }
    }

    if (pfInvalid)
        switch (mode)
        {
            case 0: // 4n base64 characters processed: ok
                break;

            case 1: // 4n+1 base64 character processed: impossible
                *pfInvalid = true;
                break;

            case 2: // 4n+2 base64 characters processed: require '=='
                if (left || p[0] != '=' || p[1] != '=' || decode64_table[(unsigned char)p[2]] != -1)
                    *pfInvalid = true;
                break;

            case 3: // 4n+3 base64 characters processed: require '='
                if (left || p[0] != '=' || decode64_table[(unsigned char)p[1]] != -1)
                    *pfInvalid = true;
                break;
        }

    return vchRet;
}

string DecodeBase64(const string& str)
{
    vector<unsigned char> vchRet = DecodeBase64(str.c_str());
    return string((const char*)&vchRet[0], vchRet.size());
}

string EncodeBase32(const unsigned char* pch, size_t len)
{
    static const char *pbase32 = "abcdefghijklmnopqrstuvwxyz234567";

    string strRet="";
    strRet.reserve((len+4)/5*8);

    int mode=0, left=0;
    const unsigned char *pchEnd = pch+len;

    while (pch<pchEnd)
    {
        int enc = *(pch++);
        switch (mode)
        {
            case 0: // we have no bits
                strRet += pbase32[enc >> 3];
                left = (enc & 7) << 2;
                mode = 1;
                break;

            case 1: // we have three bits
                strRet += pbase32[left | (enc >> 6)];
                strRet += pbase32[(enc >> 1) & 31];
                left = (enc & 1) << 4;
                mode = 2;
                break;

            case 2: // we have one bit
                strRet += pbase32[left | (enc >> 4)];
                left = (enc & 15) << 1;
                mode = 3;
                break;

            case 3: // we have four bits
                strRet += pbase32[left | (enc >> 7)];
                strRet += pbase32[(enc >> 2) & 31];
                left = (enc & 3) << 3;
                mode = 4;
                break;

            case 4: // we have two bits
                strRet += pbase32[left | (enc >> 5)];
                strRet += pbase32[enc & 31];
                mode = 0;
        }
    }

    static const int nPadding[5] = {0, 6, 4, 3, 1};
    if (mode)
    {
        strRet += pbase32[left];
        for (int n=0; n<nPadding[mode]; n++)
             strRet += '=';
    }

    return strRet;
}

string EncodeBase32(const string& str)
{
    return EncodeBase32((const unsigned char*)str.c_str(), str.size());
}

vector<unsigned char> DecodeBase32(const char* p, bool* pfInvalid)
{
    static const int decode32_table[256] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, -1, -1, -1, -1,
        -1, -1, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1,  0,  1,  2,
         3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 25, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    if (pfInvalid)
        *pfInvalid = false;

    vector<unsigned char> vchRet;
    vchRet.reserve((strlen(p))*5/8);

    int mode = 0;
    int left = 0;

    for ( ; ; )
    {
         int dec = decode32_table[(unsigned char)*p];
         if (dec == -1) break;
         p++;
         switch (mode)
         {
             case 0: // we have no bits and get 5
                 left = dec;
                 mode = 1;
                 break;

              case 1: // we have 5 bits and keep 2
                  vchRet.push_back((left<<3) | (dec>>2));
                  left = dec & 3;
                  mode = 2;
                  break;

             case 2: // we have 2 bits and keep 7
                 left = left << 5 | dec;
                 mode = 3;
                 break;

             case 3: // we have 7 bits and keep 4
                 vchRet.push_back((left<<1) | (dec>>4));
                 left = dec & 15;
                 mode = 4;
                 break;

             case 4: // we have 4 bits, and keep 1
                 vchRet.push_back((left<<4) | (dec>>1));
                 left = dec & 1;
                 mode = 5;
                 break;

             case 5: // we have 1 bit, and keep 6
                 left = left << 5 | dec;
                 mode = 6;
                 break;

             case 6: // we have 6 bits, and keep 3
                 vchRet.push_back((left<<2) | (dec>>3));
                 left = dec & 7;
                 mode = 7;
                 break;

             case 7: // we have 3 bits, and keep 0
                 vchRet.push_back((left<<5) | dec);
                 mode = 0;
                 break;
         }
    }

    if (pfInvalid)
        switch (mode)
        {
            case 0: // 8n base32 characters processed: ok
                break;

            case 1: // 8n+1 base32 characters processed: impossible
            case 3: //   +3
            case 6: //   +6
                *pfInvalid = true;
                break;

            case 2: // 8n+2 base32 characters processed: require '======'
                if (left || p[0] != '=' || p[1] != '=' || p[2] != '=' || p[3] != '=' || p[4] != '=' || p[5] != '=' || decode32_table[(unsigned char)p[6]] != -1)
                    *pfInvalid = true;
                break;

            case 4: // 8n+4 base32 characters processed: require '===='
                if (left || p[0] != '=' || p[1] != '=' || p[2] != '=' || p[3] != '=' || decode32_table[(unsigned char)p[4]] != -1)
                    *pfInvalid = true;
                break;

            case 5: // 8n+5 base32 characters processed: require '==='
                if (left || p[0] != '=' || p[1] != '=' || p[2] != '=' || decode32_table[(unsigned char)p[3]] != -1)
                    *pfInvalid = true;
                break;

            case 7: // 8n+7 base32 characters processed: require '='
                if (left || p[0] != '=' || decode32_table[(unsigned char)p[1]] != -1)
                    *pfInvalid = true;
                break;
        }

    return vchRet;
}

string DecodeBase32(const string& str)
{
    vector<unsigned char> vchRet = DecodeBase32(str.c_str());
    return string((const char*)&vchRet[0], vchRet.size());
}


int64_t DecodeDumpTime(const string& s)
{
    bt::ptime pt;

    for(size_t i=0; i<formats_n; ++i)
    {
        istringstream is(s);
        is.imbue(formats[i]);
        is >> pt;
        if(pt != bt::ptime()) break;
    }

    return pt_to_time_t(pt);
}

string EncodeDumpTime(int64_t nTime) {
    return DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", nTime);
}

string EncodeDumpString(const string &str) {
    stringstream ret;
    for(unsigned char c :  str) {
        if (c <= 32 || c >= 128 || c == '%') {
            ret << '%' << HexStr(&c, &c + 1);
        } else {
            ret << c;
        }
    }
    return ret.str();
}

string DecodeDumpString(const string &str) {
    stringstream ret;
    for (unsigned int pos = 0; pos < str.length(); pos++) {
        unsigned char c = str[pos];
        if (c == '%' && pos+2 < str.length()) {
            c = (((str[pos+1]>>6)*9+((str[pos+1]-'0')&15)) << 4) | 
                ((str[pos+2]>>6)*9+((str[pos+2]-'0')&15));
            pos += 2;
        }
        ret << c;
    }
    return ret.str();
}

bool WildcardMatch(const char* psz, const char* mask)
{
    for ( ; ; )
    {
        switch (*mask)
        {
        case '\0':
            return (*psz == '\0');
        case '*':
            return WildcardMatch(psz, mask+1) || (*psz && WildcardMatch(psz+1, mask));
        case '?':
            if (*psz == '\0')
                return false;
            break;
        default:
            if (*psz != *mask)
                return false;
            break;
        }
        psz++;
        mask++;
    }
}

bool WildcardMatch(const string& str, const string& mask)
{
    return WildcardMatch(str.c_str(), mask.c_str());
}


static string FormatException(exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(NULL, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "novacoin";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintException(exception* pex, const char* pszThread)
{
    string message = FormatException(pex, pszThread);
    printf("\n\n************************\n%s\n", message.c_str());
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
    strMiscWarning = message;
    throw;
}

void LogStackTrace() {
    printf("\n\n******* exception encountered *******\n");
    if (fileout)
    {
#if !defined(WIN32) && !defined(ANDROID)
        void* pszBuffer[32];
        size_t size;
        size = backtrace(pszBuffer, 32);
        backtrace_symbols_fd(pszBuffer, size, fileno(fileout));
#endif
    }
}

void PrintExceptionContinue(exception* pex, const char* pszThread)
{
    string message = FormatException(pex, pszThread);
    printf("\n\n************************\n%s\n", message.c_str());
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
    strMiscWarning = message;
}

boost::filesystem::path GetDefaultDataDir()
{
    namespace fs = boost::filesystem;
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\NovaCoin
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\NovaCoin
    // Mac: ~/Library/Application Support/NovaCoin
    // Unix: ~/.novacoin
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "NovaCoin";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    fs::create_directory(pathRet);
    return pathRet / "NovaCoin";
#else
    // Unix
    return pathRet / ".novacoin";
#endif
#endif
}

const boost::filesystem::path &GetDataDir(bool fNetSpecific)
{
    namespace fs = boost::filesystem;

    static fs::path pathCached[2];
    static CCriticalSection csPathCached;
    static bool cachedPath[2] = {false, false};

    fs::path &path = pathCached[fNetSpecific];

    // This can be called during exceptions by printf, so we cache the
    // value so we don't have to do memory allocations after that.
    if (cachedPath[fNetSpecific])
        return path;

    LOCK(csPathCached);

    if (mapArgs.count("-datadir")) {
        path = fs::system_complete(mapArgs["-datadir"]);
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (fNetSpecific && GetBoolArg("-testnet", false))
        path /= "testnet2";

    fs::create_directory(path);

    cachedPath[fNetSpecific]=true;
    return path;
}

string randomStrGen(size_t length) {
    std::mt19937 mtrand;
    mtrand.seed(static_cast<unsigned int>(time(NULL)));
    static const string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
    string result;
    result.resize(length);
    for (size_t i = 0; i < length; ++i)
        result[i] = charset[mtrand() % charset.length()];
    return result;
}

void createConf()
{
    ofstream pConf;
#if BOOST_FILESYSTEM_VERSION >= 3
    pConf.open(GetConfigFile().generic_string().c_str());
#else
    pConf.open(GetConfigFile().string().c_str());
#endif
    pConf << "rpcuser=user\nrpcpassword="
            + randomStrGen(15)
            + "\nrpcport=8344"
            + "\n#(0=off, 1=on) daemon - run in the background as a daemon and accept commands"
            + "\ndaemon=0"
            + "\n#(0=off, 1=on) server - accept command line and JSON-RPC commands"
            + "\nserver=0"
            + "\nrpcallowip=127.0.0.1";
    pConf.close();
}

boost::filesystem::path GetConfigFile()
{
    boost::filesystem::path pathConfigFile(GetArg("-conf", "novacoin.conf"));
    if (!pathConfigFile.is_complete()) pathConfigFile = GetDataDir(false) / pathConfigFile;
    return pathConfigFile;
}

void ReadConfigFile(map<string, string>& mapSettingsRet,
                    map<string, vector<string> >& mapMultiSettingsRet)
{
    boost::filesystem::ifstream streamConfig(GetConfigFile());
    if (!streamConfig.good())
    {
        createConf();
        new(&streamConfig) boost::filesystem::ifstream(GetConfigFile());
        if(!streamConfig.good())
            return;
    }

    set<string> setOptions;
    setOptions.insert("*");

    for (boost::program_options::detail::config_file_iterator it(streamConfig, setOptions), end; it != end; ++it)
    {
        // Don't overwrite existing settings so command line settings override bitcoin.conf
        string strKey = string("-") + it->string_key;
        if (mapSettingsRet.count(strKey) == 0)
        {
            mapSettingsRet[strKey] = it->value[0];
            // interpret nofoo=1 as foo=0 (and nofoo=0 as foo=1) as long as foo not set)
            InterpretNegativeSetting(strKey, mapSettingsRet);
        }
        mapMultiSettingsRet[strKey].push_back(it->value[0]);
    }
}

boost::filesystem::path GetPidFile()
{
    boost::filesystem::path pathPidFile(GetArg("-pid", "novacoind.pid"));
    if (!pathPidFile.is_complete()) pathPidFile = GetDataDir() / pathPidFile;
    return pathPidFile;
}

#ifndef WIN32
void CreatePidFile(const boost::filesystem::path &path, pid_t pid)
{
    FILE* file = fopen(path.string().c_str(), "w");
    if (file)
    {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}
#endif

bool RenameOver(boost::filesystem::path src, boost::filesystem::path dest)
{
#ifdef WIN32
    return MoveFileExA(src.string().c_str(), dest.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

void FileCommit(FILE *fileout)
{
    fflush(fileout);                // harmless if redundantly called
#ifdef WIN32
    _commit(_fileno(fileout));
#else
    fsync(fileno(fileout));
#endif
}

int GetFilesize(FILE* file)
{
    int nSavePos = ftell(file);
    int nFilesize = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        nFilesize = ftell(file);
    fseek(file, nSavePos, SEEK_SET);
    return nFilesize;
}

void ShrinkDebugFile()
{
    // Scroll debug.log if it's getting too big
    boost::filesystem::path pathLog = GetDataDir() / "debug.log";
    FILE* file = fopen(pathLog.string().c_str(), "r");
    if (file && GetFilesize(file) > 10 * 1000000)
    {
        // Restart the file with some of the end
        try {
            vector<char>* vBuf = new vector <char>(200000, 0);
            size_t nBytes = 1; //write one byte if fseek failed
            if (fseek(file, -((long)(vBuf->size())), SEEK_END) == 0)
                nBytes = fread(&vBuf->operator[](0), 1, vBuf->size(), file);
            fclose(file);
            file = fopen(pathLog.string().c_str(), "w");
            if (file)
            {
                fwrite(&vBuf->operator[](0), 1, nBytes, file);
                fclose(file);
            }
            delete vBuf;
        }
        catch (const bad_alloc& e) {
            // Bad things happen - no free memory in heap at program startup
            fclose(file);
            printf("Warning: %s in %s:%d\n ShrinkDebugFile failed - debug.log expands further", e.what(), __FILE__, __LINE__);
        }
    }
}

//
// "Never go to sea with two chronometers; take one or three."
// Our three time sources are:
//  - System clock
//  - Median of other nodes clocks
//  - The user (asking the user to fix the system clock if the first two disagree)
//

// System clock
int64_t GetTime()
{
    int64_t now = time(NULL);
    assert(now > 0);
    return now;
}

// Trusted NTP offset or median of NTP samples.
extern int64_t nNtpOffset;

// Median of time samples given by other nodes.
static int64_t nNodesOffset = numeric_limits<int64_t>::max();

// Select time offset:
int64_t GetTimeOffset()
{
    // If NTP and system clock are in agreement within 40 minutes, then use NTP.
    if (abs(nNtpOffset) < 40 * 60)
        return nNtpOffset;

    // If not, then choose between median peer time and system clock.
    if (abs(nNodesOffset) < 70 * 60)
        return nNodesOffset;

    return 0;
}

int64_t GetNodesOffset()
{
        return nNodesOffset;
}

int64_t GetAdjustedTime()
{
    return GetTime() + GetTimeOffset();
}

void AddTimeData(const CNetAddr& ip, int64_t nTime)
{
    int64_t nOffsetSample = nTime - GetTime();

    // Ignore duplicates
    static set<CNetAddr> setKnown;
    if (!setKnown.insert(ip).second)
        return;

    // Add data
    vTimeOffsets.input(nOffsetSample);
    printf("Added time data, samples %d, offset %+" PRId64 " (%+" PRId64 " minutes)\n", vTimeOffsets.size(), nOffsetSample, nOffsetSample/60);
    if (vTimeOffsets.size() >= 5 && vTimeOffsets.size() % 2 == 1)
    {
        auto nMedian = vTimeOffsets.median();
        auto vSorted = vTimeOffsets.sorted();
        // Only let other nodes change our time by so much
        if (abs(nMedian) < 70 * 60)
        {
            nNodesOffset = nMedian;
        }
        else
        {
            nNodesOffset = numeric_limits<int64_t>::max();

            static bool fDone;
            if (!fDone)
            {
                bool fMatch = false;

                // If nobody has a time different than ours but within 5 minutes of ours, give a warning
                for(auto nOffset :  vSorted)
                    if (nOffset != 0 && abs(nOffset) < 5 * 60)
                        fMatch = true;

                if (!fMatch)
                {
                    fDone = true;
                    string strMessage("Warning: Please check that your computer's date and time are correct! If your clock is wrong NovaCoin will not work properly.");
                    strMiscWarning = strMessage;
                    printf("*** %s\n", strMessage.c_str());
                    uiInterface.ThreadSafeMessageBox(strMessage+" ", string("NovaCoin"), CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION);
                }
            }
        }
        if (fDebug) {
            for(int64_t n :  vSorted)
                printf("%+" PRId64 "  ", n);
            printf("|  ");
        }
        if (nNodesOffset != numeric_limits<int64_t>::max())
            printf("nNodesOffset = %+" PRId64 "  (%+" PRId64 " minutes)\n", nNodesOffset, nNodesOffset/60);
    }
}

string FormatVersion(int nVersion)
{
    if (nVersion%100 == 0)
        return strprintf("%d.%d.%d", nVersion/1000000, (nVersion/10000)%100, (nVersion/100)%100);
    else
        return strprintf("%d.%d.%d.%d", nVersion/1000000, (nVersion/10000)%100, (nVersion/100)%100, nVersion%100);
}

string FormatFullVersion()
{
    return CLIENT_BUILD;
}

// Format the subversion field according to BIP 14 spec (https://en.bitcoin.it/wiki/BIP_0014)
string FormatSubVersion(const string& name, int nClientVersion, const vector<string>& comments)
{
    ostringstream ss;
    ss << "/";
    ss << name << ":" << FormatVersion(nClientVersion);
    if (!comments.empty())
    {
        ss << "(";
        for (const auto& st : comments)
        {
            ss << st;
            if (st == comments.back()) break;
            ss << "; ";
        }
        ss << ")";
    }
    ss << "/";
    return ss.str();
}

#ifdef WIN32
boost::filesystem::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    namespace fs = boost::filesystem;

    char pszPath[MAX_PATH] = "";

    if(SHGetSpecialFolderPathA(NULL, pszPath, nFolder, fCreate))
    {
        return fs::path(pszPath);
    }

    printf("SHGetSpecialFolderPathA() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

void runCommand(string strCommand)
{
    int nErr = ::system(strCommand.c_str());
    if (nErr)
        printf("runCommand error: system(%s) returned %d\n", strCommand.c_str(), nErr);
}

void RenameThread(const char* name)
{
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, name, 0, 0, 0);
#elif 0 && (defined(__FreeBSD__) || defined(__OpenBSD__))
    // TODO: This is currently disabled because it needs to be verified to work
    //       on FreeBSD or OpenBSD first. When verified the '0 &&' part can be
    //       removed.
    pthread_set_name_np(pthread_self(), name);

// This is XCode 10.6-and-later; bring back if we drop 10.5 support:
// #elif defined(MAC_OSX)
//    pthread_setname_np(name);

#else
    // Prevent warnings for unused parameters...
    (void)name;
#endif
}

bool NewThread(void(*pfn)(void*), void* parg)
{
    try
    {
        boost::thread(pfn, parg); // thread detaches when out of scope
    } catch(boost::thread_resource_error &e) {
        printf("Error creating thread: %s\n", e.what());
        return false;
    }
    return true;
}

string DateTimeStrFormat(const char* pszFormat, int64_t nTime)
{
    // locale takes ownership of the pointer
    locale loc(locale::classic(), new boost::posix_time::time_facet(pszFormat));
    stringstream ss;
    ss.imbue(loc);
    ss << boost::posix_time::from_time_t(nTime);
    return ss.str();
}

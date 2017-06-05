#include "legato.h"
#include "le_cfg_interface.h"
#include "interfaces.h"
#include <curl/curl.h>

#define SSL_ERROR_HELP    "Make sure your system date is set correctly (e.g. `date -s '2016-7-7'`)"
#define SSL_ERROR_HELP_2  "You can check the minimum date for this SSL cert to work using: `openssl s_client -connect httpbin.org:443 2>/dev/null | openssl x509 -noout -dates`"

static char * Url = "http://10.1.11.48/job/Legato-QA-Merged/lastBuild/api/json?tree=result";    ///<- Url has to be char * type and not an array, else curl will not perform

static char * contentExpected;
static bool exitCodeCheck;
static bool contentCheck;
static int http_code = 0;

static int seconds = 3;   ///<- Polling timer interval in seconds

static le_cfg_IteratorRef_t iteratorRef;
static le_timer_Ref_t PollingTimer;

// Header declaration
static void GPIO_Pin_Init(void);
static void CfgTreeInit(void);
static void CfgTreeSet(void);
static void CfgTreeGet(void);
static void Polling(le_timer_Ref_t timerRef);
static void TimerHandle(void);


struct MemoryStruct{
    char *memory;
    size_t size;
};

enum Result
{
    success = 10,
    failure = 10,
    aborted = 10,
    null = 5,
    unstable = 11
};

static void GreenLight
(
    void
)
{
    le_gpioPin21_SetPushPullOutput(LE_GPIOPIN21_ACTIVE_HIGH, false);
    le_gpioPin32_SetPushPullOutput(LE_GPIOPIN32_ACTIVE_HIGH, false);
    le_gpioPin7_SetPushPullOutput(LE_GPIOPIN7_ACTIVE_HIGH, true);
}

static void YellowLight
(
    void
)
{
    le_gpioPin21_SetPushPullOutput(LE_GPIOPIN21_ACTIVE_HIGH, false);
    le_gpioPin7_SetPushPullOutput(LE_GPIOPIN7_ACTIVE_HIGH, false);
    le_gpioPin32_SetPushPullOutput(LE_GPIOPIN32_ACTIVE_HIGH, true);
}

static void RedLight
(
    void
)
{
    le_gpioPin7_SetPushPullOutput(LE_GPIOPIN7_ACTIVE_HIGH, false);
    le_gpioPin32_SetPushPullOutput(LE_GPIOPIN32_ACTIVE_HIGH, false);
    le_gpioPin21_SetPushPullOutput(LE_GPIOPIN21_ACTIVE_HIGH, true);
}

/* Takes the data from bufferPtr and allocates memory to store in userData.	*/
/* The bufferPtr data holds the HTML text.									*/
/*						 													*/
static size_t WriteCallback
(
    void *bufferPtr,
    size_t size,
    size_t nbMember,
    void *userData 
)
{
	size_t realsize = size * nbMember;
	struct MemoryStruct *mem = (struct MemoryStruct *) userData;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if(mem->memory == NULL)
	{
		LE_ERROR("Not enough memory, reallocation error\n");
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), bufferPtr, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;

    return realsize;
}

static void SetFlags
(
    char *PtrRes
)
{
    enum Result status;

    memmove(PtrRes, PtrRes+8, strlen(PtrRes));  // Truncates the first 8 characters (result")
    // LE_INFO("%s", PtrRes);   //Log for debugging on console
    // LE_INFO("size of ptrres = %i", strlen(PtrRes));
    
    if( ( (status = success) == strlen(PtrRes) ) && (PtrRes[1] == 'S') )
    {
        //LE_INFO("success = %i", status);
        contentExpected = "SUCCESS";
        GreenLight();
    }
    else if( ( (status = failure) == strlen(PtrRes) ) && (PtrRes[1] == 'F') )
    {
        //LE_INFO("failure = %i", status);
        contentExpected = "FAILURE";
        RedLight();
    }
    else if( ( (status = failure) == strlen(PtrRes) ) && (PtrRes[1] == 'A') )
    {
        //LE_INFO("aborted = %i", status);
        contentExpected = "ABORTED";
        YellowLight();
    }
    else if( (status = null) == strlen(PtrRes) )
    {
        //LE_INFO("null = %i", status);
        contentExpected = "NULL";
        YellowLight();
    }
    else if( (status = unstable) == strlen(PtrRes) )
    {
        //LE_INFO("unstable = %i", status);
        contentExpected = "UNSTABLE";
        YellowLight();
    }
    else
    {
        LE_ERROR("Check the length of PtrRes and corresponding Result statuses");
        return;
    }

    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
    le_cfg_SetString(iteratorRef, "contentResult", contentExpected);
    le_cfg_CommitTxn(iteratorRef);

    LE_INFO("contentExpected: %s",contentExpected);
}

static void GetResult
(
    struct MemoryStruct buffer
)
{
    char *CheckString = "result";   	///<- Check for the string 'result' in the html file
    char *PtrRes;						///<- string that prints the characters following result

    PtrRes = strstr(buffer.memory, CheckString);
    
    if( PtrRes != NULL )
    {
        SetFlags(PtrRes);
    }
    else
    {
        contentExpected = "Not a valid Url for checking results!";
        LE_ERROR("contentExpected: %s",contentExpected);
    }
}

static void GetHTTPCode
(
    CURL *curl
)
{
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetInt(iteratorRef, "Result", http_code);
    le_cfg_CommitTxn(iteratorRef);

    LE_INFO("exitCodeExpected (http_code): %i", http_code);     // For debugging log
}

static void GetUrl
(
    void
)
{
    CURL *curl;					///<- Easy handle necessary for curl functions
    CURLcode res;				///<- Stores results of curl functions

    struct MemoryStruct buffer; ///<- Store size and data in buffer to be analyzed
    buffer.memory = malloc(1);
    buffer.size = 0;

    //-----------------------Should be implemented in CfgTreeGet. Problem: Global Url is not being overwritten -------//
    char stringBuffer[100] = { 0 };
    char * ptrBuffer = stringBuffer;    ///<- Cast pointer to stringBuffer to match type with Url.

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/");
    le_cfg_GetString(iteratorRef, "Url", stringBuffer, sizeof(stringBuffer), Url);
    le_cfg_CancelTxn(iteratorRef);

    Url = ptrBuffer;

    LE_INFO("Url: %s", Url);
    //-----------------------------------------------------------------------------------------------------------------//

    curl = curl_easy_init();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, Url);

        //Write data into @buffer. The conditionals check for errors
        if( (res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback) ) == CURLE_WRITE_ERROR){
        	LE_ERROR("curlopt_writefunction failed: %s", curl_easy_strerror(res));
        }
        if( (res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &buffer) ) != CURLE_OK){
        	LE_ERROR("curlopt_writedata failed: %s", curl_easy_strerror(res));
        }

        //curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 2L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            LE_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
            if (res == CURLE_SSL_CACERT)
            {
                LE_ERROR(SSL_ERROR_HELP);
                LE_ERROR(SSL_ERROR_HELP_2);
            }
        }

        // TrafficLight:/info/exitCode/CheckFlag to determine whether to check HTTPcode or not
        LE_INFO("exitCodeCheck: %s", exitCodeCheck ? "true" : "false");
        if(exitCodeCheck)
        {
            GetHTTPCode(curl);
        }
        else
        {
            CfgTreeSet();
        }
        // TrafficLight:/info/content/CheckFlag to determine whether to check result status or not
        LE_INFO("contentCheck: %s", contentCheck ? "true" : "false");
        if(contentCheck)
        {
            GetResult(buffer);
        }
        else
        {
            CfgTreeSet();
        }

        curl_easy_cleanup(curl);
    }
    else
    {
        LE_ERROR("Couldn't initialize cURL.");
    }

    curl_global_cleanup();
}

static void CfgTreeInit
(
    void
)
{
    // Set default Url to the global variable Url
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/");
    le_cfg_SetString(iteratorRef, "Url", Url);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/exitCode/CheckFlag
    Description: Settable flag to check the http_code
    Default: true
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetBool(iteratorRef, "CheckFlag", true);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/exitCode/Result
    Description: Writes the result of the httpCode. Error if http_code != 200
    Default: 0
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetInt(iteratorRef, "Result", 0);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/content/CheckFlag
    Description: Settable flag to check the result of job status
    Default: true
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
    le_cfg_SetBool(iteratorRef, "CheckFlag", true);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/content/contentResult
    Description: Writes the result of job status. Outputs: SUCCESS, FAILURE, ABORTED, NULL, or UNSTABLE
    Default: 
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
    le_cfg_SetString(iteratorRef, "contentResult", "");
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/PollIntervalSec
    Description: Polling timer intervals in second
    Default: 3 seconds
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/");
    le_cfg_SetInt(iteratorRef, "PollIntervalSec", 3);
    le_cfg_CommitTxn(iteratorRef);
}

static void CfgTreeSet
(
    void
)
{
    if(!exitCodeCheck)
    {
        iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
        le_cfg_SetInt(iteratorRef, "Result", 0);
        le_cfg_CommitTxn(iteratorRef);
    }
    if(!contentCheck)
    {
        iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
        le_cfg_SetString(iteratorRef, "contentResult", "");
        le_cfg_CommitTxn(iteratorRef);
    }
    return;
}

static void CfgTreeGet
(
    void
)
{
    int timerset;
    // char stringBuffer[100] = { 0 };
    // // char * c = stringBuffer;

    // iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/");
    // le_cfg_GetString(iteratorRef, "Url", stringBuffer, sizeof(stringBuffer), Url);
    // le_cfg_CancelTxn(iteratorRef);

    // LE_INFO("Url is %s, strlen(url)=%i, sizeof(url)= %i, ", Url, strlen(Url), sizeof(Url) );
    // LE_INFO("Url is %s, strlen(url)=%i, sizeof(url)= %i, ", stringBuffer, strlen(stringBuffer), sizeof(stringBuffer) );
    // LE_INFO("Url is %s, strlen(url)=%i, sizeof(url)= %i, ", c, strlen(c), sizeof(c) );

    // TrafficLight:/exitCode/CheckFlag to determine whether to check HTTPcode or not
    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/info/exitCode");
    exitCodeCheck = le_cfg_GetBool(iteratorRef, "CheckFlag", false);
    le_cfg_CancelTxn(iteratorRef);

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/info/content");
    contentCheck = le_cfg_GetBool(iteratorRef, "CheckFlag", false);
    le_cfg_CancelTxn(iteratorRef);

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/");
    timerset = le_cfg_GetInt(iteratorRef, "PollIntervalSec", seconds);
    le_cfg_CancelTxn(iteratorRef);

    if(timerset != seconds)
    {
        seconds = timerset;
        TimerHandle();
    }
}

static void GPIO_Pin_Init
(
    void
)
{
    le_gpioPin21_Activate();
    le_gpioPin21_EnablePullUp();

    le_gpioPin32_Activate();
    le_gpioPin32_EnablePullUp();

    le_gpioPin7_Activate();
    le_gpioPin7_EnablePullUp();

    // Set all output to logic 0 
    le_gpioPin21_SetPushPullOutput(LE_GPIOPIN21_ACTIVE_HIGH, false);
    le_gpioPin32_SetPushPullOutput(LE_GPIOPIN32_ACTIVE_HIGH, false);
    le_gpioPin7_SetPushPullOutput(LE_GPIOPIN7_ACTIVE_HIGH, false);
    LE_INFO("Pin21 read PP - High: %d", le_gpioPin21_Read());
    LE_INFO("Pin32 read PP - High: %d", le_gpioPin32_Read());
    LE_INFO("Pin7 read PP - High: %d", le_gpioPin7_Read());
}

/* 
Description: 	1. Initialize the indefinitely repeating PollingTimer to x seconds
				2. Reset polling interval from CfgTreeGet
Default: seconds = 3;
*/
static void TimerHandle
(
    void
)
{
	if(le_timer_IsRunning(PollingTimer))
	{
		le_timer_Stop(PollingTimer);
	}
	LE_INFO("SECONDS IS %i", seconds);
    le_clk_Time_t interval = {seconds, 0}; //first parameter is the seconds
    le_timer_SetInterval(PollingTimer, interval);
    le_timer_SetRepeat(PollingTimer, 0); // repeat indefinitely
    le_timer_SetHandler(PollingTimer, Polling);
    le_timer_Start(PollingTimer);
}

static void Polling
(
    le_timer_Ref_t timerRef
)
{
    LE_INFO("-------------------------- In polling function--------------------");
    CfgTreeGet();
    GetUrl();
}

//---------------------------------------------------
/**
 
 */
//---------------------------------------------------
COMPONENT_INIT
{
    curl_global_init(CURL_GLOBAL_ALL);
    GPIO_Pin_Init();
    CfgTreeInit();

    PollingTimer = le_timer_Create("PollingTimer");  //maybe can move to TimerHandle() function somehow?
    TimerHandle();
}
#include "legato.h"
#include "le_cfg_interface.h"
#include "interfaces.h"
#include <curl/curl.h>

#define SSL_ERROR_HELP    "Make sure your system date is set correctly (e.g. `date -s '2016-7-7'`)"
#define SSL_ERROR_HELP_2  "You can check the minimum date for this SSL cert to work using: `openssl s_client -connect httpbin.org:443 2>/dev/null | openssl x509 -noout -dates`"

// Default Url that is settable through config tree
static char * Url = "";

// Flags
static bool exitCodeCheck;
static bool contentCheck;

// Polling timer interval in seconds
static int seconds = 3;

static le_cfg_IteratorRef_t iteratorRef;
static le_timer_Ref_t PollingTimer;

// Header declaration
static void GPIO_Pin_Init(void);
static void CfgTreeInit(void);
static void CfgTreeSet(void);
static void CfgTreeGet(void);
static void Polling(le_timer_Ref_t timerRef);
static void TimerHandle(void);

//--------------------------------------------------------------------------------------------------
/**
 * Holds information and size of a string
 */
//--------------------------------------------------------------------------------------------------
struct MemoryStruct
{
    char *memory;
    size_t size;
};

//--------------------------------------------------------------------------------------------------
/**
 * Light statuses

 * @note Enumerated type is used in SetLightState function
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    LIGHT_OFF,
    LIGHT_RED,
    LIGHT_YELLOW,
    LIGHT_GREEN,
} LightState_t;

//--------------------------------------------------------------------------------------------------
/**
 * Sets the GPIO pins to active_high with respect to the light states.

 * @return
 *      GPIO pins
 */
//--------------------------------------------------------------------------------------------------
static void SetLightState
(
    LightState_t state    ///< [IN] States of the lights.
)
{
    bool gpioRed = false;
    bool gpioYellow = false;
    bool gpioGreen = false;

    switch(state)
    {
        case LIGHT_GREEN:
            gpioGreen = true;
            break;
        case LIGHT_YELLOW:
            gpioYellow = true;
            break;
        case LIGHT_RED:
            gpioRed = true;
            break;
        case LIGHT_OFF:
            break;
        default:
            break;
    }
    le_gpioGREEN_SetPushPullOutput(LE_GPIOGREEN_ACTIVE_HIGH, gpioGreen);
    le_gpioYELLOW_SetPushPullOutput(LE_GPIOYELLOW_ACTIVE_HIGH, gpioYellow);
    le_gpioRED_SetPushPullOutput(LE_GPIORED_ACTIVE_HIGH, gpioRed);
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Takes the data from bufferPtr and allocates memory to store in userData.

 * 2. The bufferPtr data holds the HTML text.

 * @return
 *      size and information of the data that was received
 */
//--------------------------------------------------------------------------------------------------
static size_t WriteCallback
(
    void *bufferPtr,      ///< [IN] Ptr to the string of information.
    size_t size,          ///< [IN] size of individual elements
    size_t nbMember,      ///< [IN] number of elements in bufferPtr
    void *userData        ///< [OUT] memory containing information and size
)
{
    size_t realsize = size * nbMember;
    struct MemoryStruct *mem = (struct MemoryStruct *) userData;

    LE_INFO("size = %i", size);
    LE_INFO("nbMember = %i", nbMember);

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

//--------------------------------------------------------------------------------------------------
/**
 * Set the light states by finding the respective keyword in the buffer

 * @return
 *      light states
 *      contentResult in config tree
 */
//--------------------------------------------------------------------------------------------------
static void GetResult
(
    struct MemoryStruct buffer     ///< [IN] Data that was handled in GetUrl with WriteCallback
)
{
    char * contentResult;

    if( strstr(buffer.memory, "SUCCESS") )
    {
        contentResult = "SUCCESS";
        SetLightState(LIGHT_GREEN);
    }
    else if( strstr(buffer.memory, "FAILURE") )
    {
        contentResult = "FAILURE";
        SetLightState(LIGHT_RED);
    }
    else if( strstr(buffer.memory, "ABORTED") )
    {
        contentResult = "ABORTED";
        SetLightState(LIGHT_YELLOW);
    }
    else if( strstr(buffer.memory, "UNSTABLE") )
    {
        contentResult = "UNSTABLE";
        SetLightState(LIGHT_YELLOW);
    }
    else
    {
        contentResult = "NULL";
        LE_ERROR("Cannot find keyword for statuses");
        SetLightState(LIGHT_RED);
    }

    LE_INFO("contentResult = %s", contentResult);

    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
    le_cfg_SetString(iteratorRef, "contentResult", contentResult);
    le_cfg_CommitTxn(iteratorRef);
}

//--------------------------------------------------------------------------------------------------
/**
 * Gets the HTTP code status of the Url every x seconds only if exitCodeCheck flag is true

 * @return
 *      HTTP code
 */
//--------------------------------------------------------------------------------------------------
static int GetHTTPCode
(
    CURL *curl                     ///< [IN] curl handle to perform curl functions
)
{
    int http_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetInt(iteratorRef, "result", http_code);
    le_cfg_CommitTxn(iteratorRef);

    LE_INFO("exitCodeExpected (http_code): %i", http_code);
    return http_code;
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Called by Polling function

 * 2. Tries to check the Url that is set in the config tree and store content into buffer

 * 3. Calls functions to display light depending on the boolean values of exitCodeCheck and contentCheck
 */
//--------------------------------------------------------------------------------------------------
static void GetUrl
(
    void
)
{
    CURL *curl;                         ///<- Easy handle necessary for curl functions
    CURLcode res;                       ///<- Stores results of curl functions

    struct MemoryStruct buffer;         ///<- Store size and data in buffer to be analyzed
    buffer.memory = malloc(1);
    buffer.size = 0;

    char stringBuffer[200] = { 0 };
    char * ptrBuffer = stringBuffer;    ///<- Cast pointer to stringBuffer to match type with Url.

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/");
    le_cfg_GetString(iteratorRef, "url", stringBuffer, sizeof(stringBuffer), Url);
    le_cfg_CancelTxn(iteratorRef);

    Url = ptrBuffer;

    LE_INFO("Url: %s", Url);

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

        LE_INFO("exitCodeCheck: %s", exitCodeCheck ? "true" : "false");
        LE_INFO("contentCheck: %s", contentCheck ? "true" : "false");

        // States are described in README.md
        if(exitCodeCheck)
        {
            if( GetHTTPCode(curl) == 200 )
            {
                if(contentCheck)
                {
                    GetResult(buffer);
                }
                else
                {
                    SetLightState(LIGHT_GREEN);
                }
            }
            else
            {
                SetLightState(LIGHT_RED);
            }
        }
        else
        {
            if(contentCheck)
            {
                GetResult(buffer);
            }
            else
            {
                SetLightState(LIGHT_OFF);
            }
        }
        CfgTreeSet();

        curl_easy_cleanup(curl);
    }
    else
    {
        LE_ERROR("Couldn't initialize cURL.");
    }

    curl_global_cleanup();
}

//--------------------------------------------------------------------------------------------------
/**
 * Initializes the config tree to default values when the app is first ran

 * @return
 *      config get TrafficLight:/
 */
//--------------------------------------------------------------------------------------------------
static void CfgTreeInit
(
    void
)
{
    // Set default Url to the global variable Url
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/");
    le_cfg_SetString(iteratorRef, "url", Url);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/exitCode/checkFlag
    Description: Settable flag to check the http_code
    Default: true
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetBool(iteratorRef, "checkFlag", true);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/exitCode/Result
    Description: Writes the result of the httpCode. Error if http_code != 200
    Default: 0
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
    le_cfg_SetInt(iteratorRef, "result", 0);
    le_cfg_CommitTxn(iteratorRef);

    /* config get TrafficLight:/info/content/CheckFlag
    Description: Settable flag to check the result of job status
    Default: true
    */
    iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/content");
    le_cfg_SetBool(iteratorRef, "checkFlag", true);
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
    le_cfg_SetInt(iteratorRef, "pollIntervalSec", seconds);
    le_cfg_CommitTxn(iteratorRef);
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Set exitCode to 0 if exitCodeCheck is false

 * 2. Set contentResult to NULL if contentCheck is false
 */
//--------------------------------------------------------------------------------------------------
static void CfgTreeSet
(
    void
)
{
    if(!exitCodeCheck)
    {
        iteratorRef = le_cfg_CreateWriteTxn("TrafficLight:/info/exitCode");
        le_cfg_SetInt(iteratorRef, "result", 0);
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

//--------------------------------------------------------------------------------------------------
/**
 * Called by Polling function to update states from the config tree

 * @return
 *      config get TrafficLight:/
 */
//--------------------------------------------------------------------------------------------------
static void CfgTreeGet
(
    void
)
{
    int timerset;

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/info/exitCode");
    exitCodeCheck = le_cfg_GetBool(iteratorRef, "checkFlag", false);
    le_cfg_CancelTxn(iteratorRef);

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/info/content");
    contentCheck = le_cfg_GetBool(iteratorRef, "checkFlag", false);
    le_cfg_CancelTxn(iteratorRef);

    iteratorRef = le_cfg_CreateReadTxn("TrafficLight:/");
    timerset = le_cfg_GetInt(iteratorRef, "pollIntervalSec", seconds);
    le_cfg_CancelTxn(iteratorRef);

    if(timerset != seconds)
    {
        seconds = timerset;
        TimerHandle();
    }
}

//--------------------------------------------------------------------------------------------------
/**
 * Initializes IoT pins 13, 15, 17 (green, yellow, red respectively) for output

 * @return
 *      Activated pins and initialize light to be off
 */
//--------------------------------------------------------------------------------------------------
static void GPIO_Pin_Init
(
    void
)
{
    le_gpioRED_Activate();
    le_gpioRED_EnablePullUp();

    le_gpioYELLOW_Activate();
    le_gpioYELLOW_EnablePullUp();

    le_gpioGREEN_Activate();
    le_gpioGREEN_EnablePullUp();

    SetLightState(LIGHT_OFF);

    LE_INFO("RED read PP - High: %d", le_gpioRED_Read());
    LE_INFO("YELLOW read PP - High: %d", le_gpioYELLOW_Read());
    LE_INFO("GREEN read PP - High: %d", le_gpioGREEN_Read());
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Initialize the indefinitely repeating PollingTimer to x seconds

 * 2. Reset polling interval from CfgTreeGet

 * @return
 *      A timer reference PollingTimer
 */
//--------------------------------------------------------------------------------------------------
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
    le_clk_Time_t interval = {seconds, 0}; // first parameter is the seconds
    le_timer_SetInterval(PollingTimer, interval);
    le_timer_SetRepeat(PollingTimer, 0); // repeat indefinitely
    le_timer_SetHandler(PollingTimer, Polling);
    le_timer_Start(PollingTimer);
}

//--------------------------------------------------------------------------------------------------
/**
 * This function is called every x seconds where x can be set in the configTree.
   eg. $config set TrafficLight:/pollIntervalSec 10 int
 */
//--------------------------------------------------------------------------------------------------
static void Polling
(
    le_timer_Ref_t timerRef      ///< [IN] timer reference for changing intervals, starting and stopping
)
{
    LE_INFO("-------------------------- In polling function--------------------");
    CfgTreeGet();
    GetUrl();
}

//--------------------------------------------------------------------------------------------------
/**
 * Handles internal states of GPIO pins when the app is terminated by the user.

 * @return
 *      Deactivated GPIO pins
 */
//--------------------------------------------------------------------------------------------------
static void SigChildEventHandler(int sigNum)
{
    LE_INFO("Deactivating GPIO Pins");
    le_gpioGREEN_Deactivate();
    le_gpioYELLOW_Deactivate();
    le_gpioRED_Deactivate();
}

//---------------------------------------------------
/**
 Initializes GPIO Pins, and ConfigTree
 */
//---------------------------------------------------
COMPONENT_INIT
{
    le_sig_Block(SIGTERM);
    le_sig_SetEventHandler(SIGTERM, SigChildEventHandler);

    curl_global_init(CURL_GLOBAL_ALL);
    GPIO_Pin_Init();
    CfgTreeInit();

    PollingTimer = le_timer_Create("PollingTimer");
    TimerHandle();
}
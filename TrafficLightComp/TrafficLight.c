#include "legato.h"
#include "le_cfg_interface.h"
#include <curl/curl.h>

#define SSL_ERROR_HELP    "Make sure your system date is set correctly (e.g. `date -s '2016-7-7'`)"
#define SSL_ERROR_HELP_2  "You can check the minimum date for this SSL cert to work using: `openssl s_client -connect httpbin.org:443 2>/dev/null | openssl x509 -noout -dates`"

static const char * Url = "http://jenkins-legato/job/Legato-QA-Merged/lastBuild/api/json?tree=result";
// http://jenkins-legato/ or http://10.1.11.48 //Mdm9x06-Manifest-Merged

struct MemoryStruct{
	char *memory;
	size_t size;
};

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

static void GetResult
(
	struct MemoryStruct buffer
)
{
	char *CheckString = "result";   	///<- Check for the string 'result' in the html file
	char *PtrRes;						///<- string that prints the characters following result

    PtrRes = strstr(buffer.memory, CheckString);

    LE_INFO("%s", PtrRes);
}

static void GetUrl
(
    void
)
{
    CURL *curl;					///<- Easy handle necessary for curl functions
    CURLcode res;				///<- Stores results of curl functions
	long http_code = 0;

	struct MemoryStruct buffer; ///<- Store size and data in buffer to be analyzed
	buffer.memory = malloc(1);
	buffer.size = 0;

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
        else
        {
        	GetResult(buffer);
        }

        // getting http code
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if(http_code != 200)
		{
			LE_ERROR("Bad HTTP Code: %li", http_code);
		}
		else
		{
			LE_INFO("HTTP CODE IS: %li", http_code);
		}

        curl_easy_cleanup(curl);
    }
    else
    {
        LE_ERROR("Couldn't initialize cURL.");
    }

    curl_global_cleanup();
}
//---------------------------------------------------
/**
 
 */
//---------------------------------------------------
COMPONENT_INIT
{
	curl_global_init(CURL_GLOBAL_ALL);  //maybe parameter can just be CURL_GLOBAL_SSL

 	GetUrl();
    
    le_cfg_IteratorRef_t iteratorRef = le_cfg_CreateReadTxn("/");
    bool myBoolVal = le_cfg_GetBool(iteratorRef, "TestBool", false);
    if (myBoolVal)
    {
        LE_INFO("The test value was set.");
    }
    else
    {
        LE_INFO("The test value was not set.");
    }
}
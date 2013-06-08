#include <curl/curl.h>
#include <assert.h>
#include <libxml/SAX2.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libnotify/notify.h>

#define CONFIG_FILE ".rssrc"
#define LEN_SITE_NAME 32
#define LEN_URL 128
#define LEN_ETAG 128
#define LEN_MAX 128

typedef struct
{
  time_t last_pubDate;
  char site_name[LEN_SITE_NAME];
  char etag[LEN_ETAG];
  char url[LEN_URL];
} rss_thread;

struct MemStruct
{
  char *mem;
  size_t size;
};

typedef struct
{
  char state;   // 1:title 2:pubDate
  char is_ok;
  unsigned int depth;
  struct MemStruct content;
  time_t last_pubDate;
  rss_thread* rssthread;
  NotifyNotification* notification;
} xml_node;

static void rss_on_startelem(void* ctx, const xmlChar* name, const xmlChar** atts);
static void rss_on_endelem(void* ctx, const xmlChar* name);
static void rss_on_characters(void* ctx, const xmlChar* ch, int len);
static void rss_on_enddoc(void* ctx);

static void write_config(xml_node* );

//global
char path[32];

void
die(const char* err_msg, int err_no)
{
  printf(err_msg);
  printf("\n");
  exit(err_no);
}

size_t
parse_rss_callback(char* in, size_t size, size_t nmemb, void* storage)
{
  xmlParserCtxt* ctxt = storage;
  xml_node* curr_node = ctxt->userData;
  if(curr_node->is_ok == 0)
    return(0);
  if(xmlParseChunk(ctxt, in, size*nmemb, 0) == 0)
      return(size*nmemb);
  xmlParserError(ctxt, "xmlParseChunk");
  return(0);
}

int
fetch(rss_thread* rssthread)
{
  CURL *curl = curl_easy_init();
  CURLcode res;
  notify_init(rssthread->site_name);

  xml_node current;
  memset(&current, 0, sizeof(xml_node));
  current.is_ok = 1;
  current.last_pubDate = rssthread->last_pubDate;
  current.rssthread = rssthread;

  xmlSAXHandler sax_handler;
  memset(&sax_handler, 0, sizeof(xmlSAXHandler));
  sax_handler.initialized = XML_SAX2_MAGIC;
  sax_handler.startElement= rss_on_startelem;
  sax_handler.endElement= rss_on_endelem;
  sax_handler.characters = rss_on_characters;
  sax_handler.endDocument = rss_on_enddoc;

  xmlParserCtxt* parse_context = xmlCreatePushParserCtxt(&sax_handler, &current, 0, 0, NULL);

  if(curl == NULL)
    die("Curl init error.", 1);
  curl_easy_setopt(curl, CURLOPT_URL, rssthread->url);
  //curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 40960);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, parse_rss_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, parse_context);

  if((res = curl_easy_perform(curl)) != CURLE_OK)
    die("Curl fetch error.", 1);
  else
    xmlParseChunk(parse_context, NULL, 0, 1);

  free(current.content.mem);
  return(0);
}

static void
rss_on_startelem(void* ctx, const xmlChar* name, const xmlChar** attr)
{
  xml_node* curr_node = ctx;
  //clean up
  free(curr_node->content.mem);
  curr_node->content.mem = NULL;
  curr_node->content.size = 0;
  curr_node->depth++;
  if(curr_node->depth != 4)
    return;
  if(!xmlStrcmp(name, "title") )
    curr_node->state = 1; //set lsb to 1
  else if(!xmlStrcmp(name, "pubDate"))
    curr_node->state = 2; //set 2 lsb to 1
}

static void
rss_on_endelem(void* ctx, const xmlChar* name)
{
  xml_node* curr_node = ctx;
  curr_node->depth--;
  char* mem_addr = curr_node->content.mem; //pass value

  if(curr_node->state == 1)
  {
#ifdef DEBUG
    printf("%s\n", mem_addr);
#endif
    curr_node->notification = notify_notification_new(curr_node->rssthread->site_name, NULL, NULL);
    notify_notification_update(curr_node->notification, curr_node->rssthread->site_name, mem_addr,  NULL);
    notify_notification_show(curr_node->notification, NULL);
  }
  curr_node->state = 0;
}

static void
rss_on_characters(void* ctx, const xmlChar* ch, int len)
{
  xml_node* curr_node = ctx;
  char** mem_addr = &curr_node->content.mem; //pass address
  size_t* mem_size = &curr_node->content.size;//pass address
  time_t pubDate;
  char time[32];

  switch(curr_node->state)
  {
    case 1:
    if((*mem_addr = realloc(*mem_addr, sizeof(char)*(*mem_size + len + 1))) == NULL)
    {
      printf("OOM.\n");
      curr_node->is_ok = 0;
      return;
    }
    memcpy(*mem_addr+sizeof(char)*(*mem_size), ch, sizeof(char)*len);
    *mem_size += len;
    (*mem_addr)[*mem_size] = 0;
    break;
    case 2:
    strncpy(time, ch, len);
    time[len] = 0;
    pubDate = curl_getdate(time, NULL);
    if(pubDate > curr_node->last_pubDate)
      curr_node->last_pubDate = pubDate;
#ifdef DEBUG
    printf("%d :: %d :: %d ----->", pubDate, curr_node->last_pubDate, curr_node->rssthread->last_pubDate);
#endif
    if(pubDate <= curr_node->rssthread->last_pubDate) //display only new feeds
    {
      curr_node->is_ok = 0;
      write_config(curr_node);
      printf("Config file written\n");
      exit(EXIT_SUCCESS);
    }
    break;
    default:
    return;
  }
}

static void
rss_on_enddoc(void* ctx)
{
  xml_node* curr_node = ctx;
  write_config(curr_node);
  printf("Config file written\n");
}

static rss_thread*
read_config(const char* filename)
{
  unsigned int i;
  rss_thread* rssthread = NULL;
  xmlDoc* doc =  xmlParseFile(filename);
  assert(doc != NULL);
  xmlXPathContext* xpath_ctx = xmlXPathNewContext(doc);
  assert(xpath_ctx != NULL);
  xmlXPathObject*  xpath_obj = xmlXPathEvalExpression("/sites/*", xpath_ctx);
  xmlXPathObject* xpath_content;
  assert(xpath_obj != NULL);

  xmlNodeSet* node_set = xpath_obj->nodesetval;
  char* nodes;
  rssthread = calloc(1, sizeof(rss_thread));
  for(i = 0; i < node_set->nodeNr; i++)
  {
    strncpy(rssthread->site_name, node_set->nodeTab[i]->name, LEN_SITE_NAME);
    xpath_content = xmlXPathEvalExpression("//url", xpath_ctx);
    assert(xpath_content != NULL);
    nodes = xpath_content->nodesetval->nodeTab[0]->children->content;
    strncpy(rssthread->url, nodes, LEN_URL);

    xpath_content = xmlXPathEvalExpression("//last_pubDate", xpath_ctx);
    assert(xpath_content != NULL);
    nodes = xpath_content->nodesetval->nodeTab[0]->children->content;
    rssthread->last_pubDate = atoi(nodes);
  }

  xmlXPathFreeObject(xpath_obj);
  xmlXPathFreeContext(xpath_ctx);

  return(rssthread);
}

static void
write_config(xml_node* xmlnode)
{
  char exp[LEN_SITE_NAME+7] = "/sites/\0";
  strncpy(exp, xmlnode->rssthread->site_name, LEN_SITE_NAME);
  xmlDoc* doc =  xmlParseFile(path);
  assert(doc != NULL);
  xmlXPathContext* xpath_ctx = xmlXPathNewContext(doc);
  assert(xpath_ctx != NULL);
  xmlXPathObject*  xpath_obj = xmlXPathEvalExpression(exp, xpath_ctx);
  xmlXPathObject* xpath_content;
  assert(xpath_obj != NULL);
  xmlNodeSet* node_set = xpath_obj->nodesetval;
  xmlNode* nodes;

  xpath_content = xmlXPathEvalExpression("//last_pubDate", xpath_ctx);
  assert(xpath_content != NULL);
  nodes = xpath_content->nodesetval->nodeTab[0];
  sprintf(exp, "%d", xmlnode->last_pubDate);
  xmlNodeSetContent(nodes, exp);
  xmlSaveFile(path, doc);
  xmlXPathFreeObject(xpath_obj);
  xmlXPathFreeContext(xpath_ctx);
}

int
main()
{
  //Curl for rss fetch
  sprintf(path, "%s/%s", getenv("HOME"), CONFIG_FILE);
  xmlInitParser();
  rss_thread* rssthread = read_config(path);
  /*
  printf("%s\n", rssthread->site_name);
  printf("%s\n", rssthread->url);
  */
  fetch(rssthread);

  free(rssthread);
  xmlCleanupParser();
  xmlMemoryDump();
  return(EXIT_SUCCESS);
}


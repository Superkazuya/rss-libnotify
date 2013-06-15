// Copyright (C) 
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
// 

#include <curl/curl.h>
#include <assert.h>
#include <libxml/SAX2.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <pthread.h>
#include <unistd.h>
#include <libnotify/notify.h>
#include <iostream>
#include <string>
#include <stack>

using namespace std;

//#define CURL_DEBUG
#define CONFIG_FILE ".rssrc"
//global
static pthread_mutex_t config_mutex;
string path;

static void    *fetch(void                *);
static void     rss_on_startelem(void     *ctx, const xmlChar *name, const xmlChar**atts);
static void     rss_on_endelem(void       *ctx, const xmlChar *name);
static void     rss_on_characters(void    *ctx, const xmlChar *ch, int len);
static void     rss_on_enddoc(void        *ctx);
static xmlNode *get_node_by_name(xmlChar  *, xmlNode*);
static int      read_config(const         string);

#include "rss_info.h"
rss_info::rss_info(string site_name, string url, string last_pubDate):
  site_name(site_name), url(url), last_pubDate(stoi(last_pubDate))
{
}

thread::thread(const rss_info &rinfo):
  is_in_item(false),
  state(NULL),
  depth(0),
  info(rinfo), 
  last_pubDate(rinfo.last_pubDate)
{
  notify_init(rinfo.site_name.c_str());
  pthread_create(&thread_id, NULL, fetch, this);
}

void thread::notify_send()
{
  notification = notify_notification_new(info.site_name.c_str(), NULL, NULL);
  notify_notification_update(notification, info.site_name.c_str(), ("<a href=\""+item.link+"\">"+item.title+"</a>").c_str(),  NULL);
  notify_notification_show(notification, NULL);
}

/// @brief write config
thread::~thread()
{
  if(last_pubDate <= info.last_pubDate) //no change
    return;
  pthread_mutex_lock(&config_mutex);
  xmlDoc* doc =  xmlParseFile(path.c_str());
  assert(doc != NULL);
  xmlNode* site = xmlDocGetRootElement(doc)->children;
  assert(site != NULL);
  xmlNode* node;
  string pubDate;

  //iterator, all sites
  for( ;(site = get_node_by_name(BAD_CAST "site", site)) != NULL; site = site->next)
  {
    node = site->children;
    assert((node = get_node_by_name(BAD_CAST "name", node)) != NULL); //ONLY ONE name IS ALLOWED IN ONE site
    if(info.site_name != (char*)node->children->content)
      continue;

    assert((node = get_node_by_name(BAD_CAST "last_pubDate", node)) != NULL);
    pubDate = to_string(last_pubDate);
    xmlNodeSetContent(node->children, BAD_CAST pubDate.c_str());
  }
  xmlSaveFile(path.c_str(), doc);
  xmlFreeDoc(doc);
  pthread_mutex_unlock(&config_mutex);
  cerr << "thread " << info.last_pubDate <<":config file written."<< endl;
  return;
}

static xmlNode*
get_node_by_name(xmlChar* name, xmlNode* node_head)
{
  for( ;node_head != NULL; node_head = node_head->next)
    if(!xmlStrcmp(name, node_head->name))
      return node_head;
  return NULL;
}

size_t
parse_rss_callback(char* in, size_t size, size_t nmemb, void* storage)
{
  xmlParserCtxt* ctxt = (xmlParserCtxt*) storage;
  thread* t = (thread*)ctxt->userData;
  if(xmlParseChunk(ctxt, in, size*nmemb, 0) == 0)
      return(size*nmemb);
  xmlParserError(ctxt, "xmlParseChunk");
  return(0);
}

void*
fetch(void* t)
{
  //Curl for rss fetch
  CURL *curl = curl_easy_init();
  CURLcode res;
  thread* fetch_thread = (thread*)t;

#ifdef DEBUG
  cerr <<"thread "<< fetch_thread->thread_id <<" start: "<< fetch_thread->info.url << endl;
#endif

  xmlSAXHandler sax_handler = {0};
  sax_handler.initialized = XML_SAX2_MAGIC;
  sax_handler.startElement= rss_on_startelem;
  sax_handler.endElement= rss_on_endelem;
  sax_handler.characters = rss_on_characters;
  sax_handler.endDocument = rss_on_enddoc;

  xmlParserCtxt* parse_context = xmlCreatePushParserCtxt(&sax_handler, fetch_thread, 0, 0, NULL);

  assert(curl != NULL);
  curl_easy_setopt(curl, CURLOPT_URL, fetch_thread->info.url.c_str());
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 40960);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef CURL_DEBUG
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
#endif
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, parse_rss_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, parse_context);

  if((res = curl_easy_perform(curl)) != CURLE_OK)
    goto FETCH_ERROR;
  else
    xmlParseChunk(parse_context, NULL, 0, 1);

  curl_easy_cleanup(curl);
  pthread_exit(NULL);
FETCH_ERROR:
  cerr << "fetch error!" <<endl;
  pthread_exit(NULL);
}

static void
rss_on_startelem(void* ctx, const xmlChar* name, const xmlChar** attr)
{
  thread* t = (thread*) ctx;
  t->depth++;
  if(!xmlStrcmp(name, BAD_CAST "item") )
  {
    t->is_in_item = true;
    return;
  }
  if(t->depth != 4 || t->is_in_item == false)
    return;
  if(!xmlStrcmp(name, BAD_CAST "title") )
  {
    t->state = &t->item.title;
    t->item.title.clear();
  }
  else if(!xmlStrcmp(name, BAD_CAST "pubDate"))
  {
    t->state = &t->item.pubDate;
    t->item.pubDate.clear();
  }
  else if(!xmlStrcmp(name, BAD_CAST "link"))
  {
    t->state = &t->item.link;
    t->item.link.clear();
  }
  else
    t->state = NULL;
}

static void
rss_on_endelem(void* ctx, const xmlChar* name)
{
  thread* t = (thread*) ctx;
  t->depth--;
  //t->state = NULL; 
  time_t pubDate;
  if(!xmlStrcmp(name,BAD_CAST "item") )
  {
    t->is_in_item = false;
    pubDate = curl_getdate(t->item.pubDate.c_str(), NULL);
    if(pubDate > t->last_pubDate)
      t->last_pubDate = pubDate;
    else if(pubDate <= t->info.last_pubDate) //display only new feeds
      pthread_exit(NULL);

#ifdef DEBUG
    cerr << t->item.title<<","<< t->item.pubDate <<"( "<<pubDate<<" ). newest: " << t->last_pubDate<< endl <<"URL: "<< t->item.link << endl;
#endif

    t->notify_send();
  }
}

static void
rss_on_characters(void* ctx, const xmlChar* ch, int len)
{
  thread* t = (thread*) ctx;
  string tmp;

  if(t->is_in_item && t->state != NULL)
      *t->state += string((char*)ch, (size_t)len);
}

static void
rss_on_enddoc(void* ctx)
{
  pthread_exit(NULL);
}

static int
/// @brief Read in configuration file, create threads for each site.
/// and push their refs into a vector
/// @param filename
read_config(stack<thread*>* threads_ptr)
{
  xmlDoc* doc =  xmlParseFile(path.c_str());
  assert(doc != NULL);
  xmlNode* site = xmlDocGetRootElement(doc)->children;
  xmlNode* node;
  string site_name, url;

  //iterator, all sites
  for(;(site = get_node_by_name(BAD_CAST "site", site)) != NULL; site = site->next)
  {
    node = site->children;
    assert((node = get_node_by_name(BAD_CAST "name", node)) != NULL); //ONLY ONE name IS ALLOWED IN ONE site
    site_name = (char*)node->children->content;

    assert((node = get_node_by_name(BAD_CAST "url", node)) != NULL);
    url = (char*)node->children->content;

    assert((node = get_node_by_name(BAD_CAST "last_pubDate", node)) != NULL);
#ifdef DEBUG
    cerr << site_name <<" , " <<url<<" , "<<node->children->content <<endl;
#endif
    threads_ptr->push(new thread(rss_info(site_name, url, (string)((char*)node->children->content))));
  }
  xmlFreeDoc(doc);
  return 0;
READ_ERR:
  printf("config file error!.\n");
  return 1;
}

int
main()
{
  stack<thread*> threads;

  path = string(getenv("HOME")) +"/"+ CONFIG_FILE;
  xmlInitParser();
  pthread_mutex_init(&config_mutex, NULL);
  curl_global_init(CURL_GLOBAL_ALL);

  while(1)
  {
    assert(read_config(&threads) == 0);
    while(!threads.empty())
    {
      pthread_join(threads.top()->thread_id, NULL);
      delete threads.top();
      threads.pop();
    }
    sleep(300);
  }

  xmlCleanupParser();
  xmlMemoryDump();
  pthread_mutex_destroy(&config_mutex);
  return(EXIT_SUCCESS);
}


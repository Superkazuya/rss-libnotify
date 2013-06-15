/// @brief all information retrieved from config file
class rss_info
{
  public:
    time_t last_pubDate;
    string site_name;
    string etag;
    string url;
    rss_info(string, string, string);
}; 

class rss_item
{
  public:
    string title;
    string link;
    string pubDate; //temp
};

class thread
{
  private:
    NotifyNotification* notification;
  public:
    pthread_t thread_id;
    bool is_in_item;   
    string* state;
    time_t last_pubDate; //newest update
    unsigned int depth;
    rss_info info;
    rss_item item;
    thread(const rss_info &rinfo);
    ~thread();
    void notify_send();
}; 



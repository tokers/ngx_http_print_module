# ngx_http_print_module
Simple Nginx module for producing HTTP response


## Configuration

```nginx
server {
    listen  10086;
    server_name localhost;
    
    location /t {
        print_sep "\t";
        print_ends "!\n";
        print "Today" "is";
        print "Wednesday";
        print "I like";
        print "Nginx";
    };
}
```

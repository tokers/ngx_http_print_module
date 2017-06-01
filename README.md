# ngx_http_print_module
Simple Nginx module for producing HTTP response



## Note

This document is semi-finished.


## Configuration

```nginx
server {
    listen  10086;
    server_name localhost;
    
    location /t {
        print_flush on;
        print_sep "\t";
        print_ends "!\n";
        print "Today" "is";
        print "Wednesday";
        print "I like";
        print "Nginx";

        print_duplicate 10 1000 "Hi" "It" "is a duplicate message.";
    }
}
```

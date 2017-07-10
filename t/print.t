use lib 'lib';
use Test::Nginx::Socket;

repeat_each(2);
plan tests => repeat_each() * (2 * blocks() + 1);

no_long_string();

run_tests();

__DATA__

=== TEST 1: print directive normally.

--- config
location /t {
    print "I" "am" "Alex";
}

--- request
GET /t

--- status_code: 200
--- response_body
IamAlex
--- no_error_log
[error]

=== TEST 2: print directive with seps or ends

--- config
location /t {
    print "I" "am" "Alex";
    print_sep " ";
    print_ends "\n";
}

--- request
GET /t

--- status_code: 200
--- response_body eval
"I am Alex\n"
--- no_error_log
[error]

=== TEST 3: print_duplicate

--- config
location /t {
    print 5 200 "I" "am" "Alex";
    print_sep "\t";
    print_ends "\n";
}

--- request
GET /t

--- status_code: 200
--- response_body eval
["I\tam\tAlex\n", "I\tam\tAlex\n", "I\tam\tAlex\n", "I\tam\tAlex\n",
"I\tam\tAlex\n"]
--- no_error_log
[error]

#include <string>
 

const char* url() {
    return "scheme://not-a-comment";  
}

int main() {
    return url()[0];  
}


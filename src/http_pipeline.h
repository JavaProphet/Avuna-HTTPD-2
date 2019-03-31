//
// Created by p on 2/10/19.
//

#ifndef AVUNA_HTTPD_HTTP_PIPELINE_H
#define AVUNA_HTTPD_HTTP_PIPELINE_H

#include <avuna/vhost.h>
#include "work.h"

void generateDefaultErrorPage(struct request_session* rs, const char* msg);

int generateResponse(struct request_session* rs);


#endif //AVUNA_HTTPD_HTTP_PIPELINE_H

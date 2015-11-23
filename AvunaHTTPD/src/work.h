/*
 * work.h
 *
 *  Created on: Nov 18, 2015
 *      Author: root
 */

#ifndef WORK_H_
#define WORK_H_

#include "collection.h"
#include "accept.h"
#include "log.h"

struct work_param {
		struct collection* conns;
		int pipes[2];
		struct logsess* logsess;
};

void run_work(struct work_param* param);

#endif /* WORK_H_ */
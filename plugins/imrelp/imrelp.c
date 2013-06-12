/* imrelp.c
 *
 * This is the implementation of the RELP input module.
 *
 * File begun on 2008-03-13 by RGerhards
 *
 * Copyright 2008-2013 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <librelp.h>
#include "rsyslog.h"
#include "dirty.h"
#include "errmsg.h"
#include "cfsysline.h"
#include "module-template.h"
#include "net.h"
#include "msg.h"
#include "unicode-helper.h"
#include "prop.h"
#include "ruleset.h"
#include "glbl.h"

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imrelp")

/* static data */
DEF_IMOD_STATIC_DATA
DEFobjCurrIf(net)
DEFobjCurrIf(prop)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(ruleset)
DEFobjCurrIf(glbl)

/* forward definitions */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal);


/* Module static data */
/* config vars for legacy config system */
static relpEngine_t *pRelpEngine;	/* our relp engine */
static prop_t *pInputName = NULL;	/* there is only one global inputName for all messages generated by this module */
static struct configSettings_s {
	uchar *pszBindRuleset;		/* name of Ruleset to bind to */
} cs;

struct instanceConf_s {
	uchar *pszBindPort;		/* port to bind to */
	sbool bEnableTLS;
	sbool bEnableTLSZip;
	int dhBits;
	uchar *pristring;		/* GnuTLS priority string (NULL if not to be provided) */
	struct instanceConf_s *next;
};


struct modConfData_s {
	rsconf_t *pConf;		/* our overall config object */
	instanceConf_t *root, *tail;
	uchar *pszBindRuleset;		/* name of Ruleset to bind to */
	ruleset_t *pBindRuleset; /* due to librelp limitation, we need to bind all listerns to the same set */
};

static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for the current load process */

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {
	{ "ruleset", eCmdHdlrGetWord, 0 },
};
static struct cnfparamblk modpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
	  modpdescr
	};

/* input instance parameters */
static struct cnfparamdescr inppdescr[] = {
	{ "port", eCmdHdlrString, CNFPARAM_REQUIRED },
	{ "tls", eCmdHdlrBinary, 0 },
	{ "tls.dhbits", eCmdHdlrInt, 0 },
	{ "tls.prioritystring", eCmdHdlrString, 0 },
	{ "tls.compression", eCmdHdlrBinary, 0 }
};
static struct cnfparamblk inppblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(inppdescr)/sizeof(struct cnfparamdescr),
	  inppdescr
	};



/* ------------------------------ callbacks ------------------------------ */

/* callback for receiving syslog messages. This function is invoked from the
 * RELP engine when a syslog message arrived. It must return a relpRetVal,
 * with anything else but RELP_RET_OK terminating the relp session. Please note
 * that RELP_RET_OK is equal to RS_RET_OK and the other libRELP error codes
 * are different from our rsRetVal. So we can simply use our own iRet system
 * to fulfill the requirement.
 * rgerhards, 2008-03-21
 * Note: librelp 1.0.0 is required in order to receive the IP address, otherwise
 * we will only see the hostname (twice). -- rgerhards, 2009-10-14
 */
static relpRetVal
onSyslogRcv(uchar *pHostname, uchar *pIP, uchar *msg, size_t lenMsg)
{
	prop_t *pProp = NULL;
	msg_t *pMsg;
	DEFiRet;

	CHKiRet(msgConstruct(&pMsg));
	MsgSetInputName(pMsg, pInputName);
	MsgSetRawMsg(pMsg, (char*)msg, lenMsg);
	MsgSetFlowControlType(pMsg, eFLOWCTL_LIGHT_DELAY);
	MsgSetRuleset(pMsg, runModConf->pBindRuleset);
	pMsg->msgFlags  = PARSE_HOSTNAME | NEEDS_PARSING;

	/* TODO: optimize this, we can store it inside the session, requires
	 * changes to librelp --> next librelp iteration?. rgerhards, 2012-10-29
	 */
	MsgSetRcvFromStr(pMsg, pHostname, ustrlen(pHostname), &pProp);
	CHKiRet(prop.Destruct(&pProp));
	CHKiRet(MsgSetRcvFromIPStr(pMsg, pIP, ustrlen(pIP), &pProp));
	CHKiRet(prop.Destruct(&pProp));
	CHKiRet(submitMsg2(pMsg));

finalize_it:

	RETiRet;
}


/* ------------------------------ end callbacks ------------------------------ */

/* create input instance, set default paramters, and
 * add it to the list of instances.
 */
static rsRetVal
createInstance(instanceConf_t **pinst)
{
	instanceConf_t *inst;
	DEFiRet;
	CHKmalloc(inst = MALLOC(sizeof(instanceConf_t)));
	inst->next = NULL;

	inst->pszBindPort = NULL;
	inst->bEnableTLS = 0;
	inst->bEnableTLSZip = 0;
	inst->dhBits = 0;
	inst->pristring = NULL;

	/* node created, let's add to config */
	if(loadModConf->tail == NULL) {
		loadModConf->tail = loadModConf->root = inst;
	} else {
		loadModConf->tail->next = inst;
		loadModConf->tail = inst;
	}

	*pinst = inst;
finalize_it:
	RETiRet;
}


/* modified to work for module, not instance (as usual) */
static inline void
std_checkRuleset_genErrMsg(modConfData_t *modConf, __attribute__((unused)) instanceConf_t *inst)
{
	errmsg.LogError(0, NO_ERRCODE, "imrelp: ruleset '%s' not found - "
			"using default ruleset instead", modConf->pszBindRuleset);
}


/* This function is called when a new listener instance shall be added to 
 * the current config object via the legacy config system. It just shuffles
 * all parameters to the listener in-memory instance.
 * rgerhards, 2011-05-04
 */
static rsRetVal addInstance(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	instanceConf_t *inst;
	DEFiRet;

	CHKiRet(createInstance(&inst));

	if(pNewVal == NULL || *pNewVal == '\0') {
		errmsg.LogError(0, NO_ERRCODE, "imrelp: port number must be specified, listener ignored");
	}
	inst->pszBindPort = pNewVal;

finalize_it:
	RETiRet;
}


static rsRetVal
addListner(modConfData_t __attribute__((unused)) *modConf, instanceConf_t *inst)
{
	relpSrv_t *pSrv;
	DEFiRet;
	if(pRelpEngine == NULL) {
		CHKiRet(relpEngineConstruct(&pRelpEngine));
		CHKiRet(relpEngineSetDbgprint(pRelpEngine, dbgprintf));
		CHKiRet(relpEngineSetFamily(pRelpEngine, glbl.GetDefPFFamily()));
		CHKiRet(relpEngineSetEnableCmd(pRelpEngine, (uchar*) "syslog", eRelpCmdState_Required));
		CHKiRet(relpEngineSetSyslogRcv(pRelpEngine, onSyslogRcv));
		if (!glbl.GetDisableDNS()) {
			CHKiRet(relpEngineSetDnsLookupMode(pRelpEngine, 1));
		}
	}

	CHKiRet(relpEngineListnerConstruct(pRelpEngine, &pSrv));
	CHKiRet(relpSrvSetLstnPort(pSrv, inst->pszBindPort));
	if(inst->bEnableTLS) {
		relpSrvEnableTLS(pSrv);
		if(inst->bEnableTLSZip) {
			relpSrvEnableTLSZip(pSrv);
		}
		if(inst->dhBits) {
			relpSrvSetDHBits(pSrv, inst->dhBits);
		}
		relpSrvSetGnuTLSPriString(pSrv, (char*)inst->pristring);
	}
	CHKiRet(relpEngineListnerConstructFinalize(pRelpEngine, pSrv));

finalize_it:
	RETiRet;
}


BEGINnewInpInst
	struct cnfparamvals *pvals;
	instanceConf_t *inst;
	int i;
CODESTARTnewInpInst
	DBGPRINTF("newInpInst (imrelp)\n");

	pvals = nvlstGetParams(lst, &inppblk, NULL);
	if(pvals == NULL) {
		errmsg.LogError(0, RS_RET_MISSING_CNFPARAMS,
			        "imrelp: required parameter are missing\n");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if(Debug) {
		dbgprintf("input param blk in imrelp:\n");
		cnfparamsPrint(&inppblk, pvals);
	}

	CHKiRet(createInstance(&inst));

	for(i = 0 ; i < inppblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(inppblk.descr[i].name, "port")) {
			inst->pszBindPort = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(inppblk.descr[i].name, "tls")) {
			inst->bEnableTLS = (unsigned) pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "tls.dhbits")) {
			inst->dhBits = (unsigned) pvals[i].val.d.n;
		} else if(!strcmp(inppblk.descr[i].name, "tls.prioritystring")) {
			inst->pristring = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(inppblk.descr[i].name, "tls.compression")) {
			inst->bEnableTLSZip = (unsigned) pvals[i].val.d.n;
		} else {
			dbgprintf("imrelp: program error, non-handled "
			  "param '%s'\n", inppblk.descr[i].name);
		}
	}
finalize_it:
CODE_STD_FINALIZERnewInpInst
	cnfparamvalsDestruct(pvals, &inppblk);
ENDnewInpInst


BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
	pModConf->pszBindRuleset = NULL;
	pModConf->pBindRuleset = NULL;
	/* init legacy config variables */
	cs.pszBindRuleset = NULL;
ENDbeginCnfLoad


BEGINsetModCnf
	struct cnfparamvals *pvals = NULL;
	int i;
CODESTARTsetModCnf
	pvals = nvlstGetParams(lst, &modpblk, NULL);
	if(pvals == NULL) {
		errmsg.LogError(0, RS_RET_MISSING_CNFPARAMS, "error processing module "
				"config parameters [module(...)]");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if(Debug) {
		dbgprintf("module (global) param blk for imrelp:\n");
		cnfparamsPrint(&modpblk, pvals);
	}

	for(i = 0 ; i < modpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(modpblk.descr[i].name, "ruleset")) {
			loadModConf->pszBindRuleset = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else {
			dbgprintf("imrelp: program error, non-handled "
			  "param '%s' in beginCnfLoad\n", modpblk.descr[i].name);
		}
	}
finalize_it:
	if(pvals != NULL)
		cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf

BEGINendCnfLoad
CODESTARTendCnfLoad
	if(loadModConf->pszBindRuleset == NULL) {
		if((cs.pszBindRuleset == NULL) || (cs.pszBindRuleset[0] == '\0')) {
			loadModConf->pszBindRuleset = NULL;
		} else {
			CHKmalloc(loadModConf->pszBindRuleset = ustrdup(cs.pszBindRuleset));
		}
	} else {
		if((cs.pszBindRuleset != NULL) && (cs.pszBindRuleset[0] != '\0')) {
			errmsg.LogError(0, RS_RET_DUP_PARAM, "imrelp: warning: ruleset "
					"set via legacy directive ignored");
		}
	}
finalize_it:
	free(cs.pszBindRuleset);
	loadModConf = NULL; /* done loading */
ENDendCnfLoad


BEGINcheckCnf
	rsRetVal localRet;
	ruleset_t *pRuleset;
CODESTARTcheckCnf
	/* we emulate the standard "ruleset query" code provided by the framework
	 * for *instances* (which we can currently not support due to librelp).
	 */
	if(pModConf->pszBindRuleset == NULL) {
		pModConf->pBindRuleset = NULL;
	} else {
		DBGPRINTF("imrelp: using ruleset '%s'\n", pModConf->pszBindRuleset);
		localRet = ruleset.GetRuleset(pModConf->pConf, &pRuleset, pModConf->pszBindRuleset);
		if(localRet == RS_RET_NOT_FOUND) {
			std_checkRuleset_genErrMsg(pModConf, NULL);
		}
		CHKiRet(localRet);
		pModConf->pBindRuleset = pRuleset;
	}
finalize_it:
ENDcheckCnf


BEGINactivateCnfPrePrivDrop
	instanceConf_t *inst;
CODESTARTactivateCnfPrePrivDrop
	runModConf = pModConf;
	for(inst = runModConf->root ; inst != NULL ; inst = inst->next) {
		addListner(pModConf, inst);
	}
	if(pRelpEngine == NULL)
		ABORT_FINALIZE(RS_RET_NO_RUN);
finalize_it:
ENDactivateCnfPrePrivDrop

BEGINactivateCnf
CODESTARTactivateCnf
ENDactivateCnf


BEGINfreeCnf
	instanceConf_t *inst, *del;
CODESTARTfreeCnf
	for(inst = pModConf->root ; inst != NULL ; ) {
		free(inst->pszBindPort);
		del = inst;
		inst = inst->next;
		free(del);
	}
ENDfreeCnf

/* This is used to terminate the plugin. Note that the signal handler blocks
 * other activity on the thread. As such, it is safe to request the stop. When
 * we terminate, relpEngine is called, and it's select() loop interrupted. But
 * only *after this function is done*. So we do not have a race!
 */
static void
doSIGTTIN(int __attribute__((unused)) sig)
{
	DBGPRINTF("imrelp: termination requested via SIGTTIN - telling RELP engine\n");
	relpEngineSetStop(pRelpEngine);
}


/* This function is called to gather input.
 */
BEGINrunInput
	sigset_t sigSet;
	struct sigaction sigAct;
CODESTARTrunInput
	/* we want to support non-cancel input termination. To do so, we must signal librelp
	 * when to stop. As we run on the same thread, we need to register as SIGTTIN handler,
	 * which will be used to put the terminating condition into librelp.
	 */
	sigfillset(&sigSet);
	pthread_sigmask(SIG_BLOCK, &sigSet, NULL);
	sigemptyset(&sigSet);
	sigaddset(&sigSet, SIGTTIN);
	pthread_sigmask(SIG_UNBLOCK, &sigSet, NULL);
	memset(&sigAct, 0, sizeof (sigAct));
	sigemptyset(&sigAct.sa_mask);
	sigAct.sa_handler = doSIGTTIN;
	sigaction(SIGTTIN, &sigAct, NULL);

	iRet = relpEngineRun(pRelpEngine);
ENDrunInput


BEGINwillRun
CODESTARTwillRun
ENDwillRun


BEGINafterRun
CODESTARTafterRun
	/* do cleanup here */
ENDafterRun


BEGINmodExit
CODESTARTmodExit
	if(pRelpEngine != NULL)
		iRet = relpEngineDestruct(&pRelpEngine);

	/* global variable cleanup */
	if(pInputName != NULL)
		prop.Destruct(&pInputName);

	/* release objects we used */
	objRelease(ruleset, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
	objRelease(net, LM_NET_FILENAME);
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit


static rsRetVal
resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	free(cs.pszBindRuleset);
	cs.pszBindRuleset = NULL;
	return RS_RET_OK;
}


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
CODEqueryEtryPt_STD_CONF2_PREPRIVDROP_QUERIES
CODEqueryEtryPt_STD_CONF2_IMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	pRelpEngine = NULL;
	/* request objects we use */
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(net, LM_NET_FILENAME));
	CHKiRet(objUse(ruleset, CORE_COMPONENT));

	/* register config file handlers */
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputrelpserverbindruleset", 0, eCmdHdlrGetWord,
		NULL, &cs.pszBindRuleset, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"inputrelpserverrun", 0, eCmdHdlrGetWord,
				   addInstance, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
		resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));

	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.Construct(&pInputName));
	CHKiRet(prop.SetString(pInputName, UCHAR_CONSTANT("imrelp"), sizeof("imrelp") - 1));
	CHKiRet(prop.ConstructFinalize(pInputName));
ENDmodInit


/* vim:set ai:
 */

/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2003 - 2006, Aheeva Technology.
 *
 * Claude Klimos (claude.klimos@aheeva.com)
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
 */

/*! \file
 *
 * \brief Automated Dialer detection based on APP_AMD
 *
 * \author Claude Klimos (claude.klimos@aheeva.com)
 * \author Justin Zimmer (jzimmer@leasehawk.com)
 * 
 * \ingroup applications
 */

/*! \li \ref app_spit.c uses the configuration file \ref spit.conf
 * \addtogroup configuration_file Configuration Files
 */

/*! 
 * \page spit.conf spit.conf
 * \verbinclude spit.conf.sample
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/dsp.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/format_cache.h"

/*** DOCUMENTATION
	<application name="SPIT" language="en_US">
		<synopsis>
			Attempt to detect automated dialers.
		</synopsis>
		<syntax>
			<parameter name="initialSilence" required="false">
				<para>Is maximum initial silence duration before greeting.</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="greeting" required="false">
				<para>is the maximum length of a greeting.</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="afterGreetingSilence" required="false">
				<para>Is the silence after detecting a greeting.</para>
				<para>If this is exceeded set as HUMAN</para>
			</parameter>
			<parameter name="totalAnalysis Time" required="false">
				<para>Is the maximum time allowed for the algorithm</para>
				<para>to decide HUMAN or MACHINE</para>
			</parameter>
			<parameter name="miniumWordLength" required="false">
				<para>Is the minimum duration of Voice considered to be a word</para>
			</parameter>
			<parameter name="betweenWordSilence" required="false">
				<para>Is the minimum duration of silence after a word to
				consider the audio that follows to be a new word</para>
			</parameter>
			<parameter name="maximumNumberOfWords" required="false">
				<para>Is the maximum number of words in a greeting</para>
				<para>If this is exceeded set as MACHINE</para>
			</parameter>
			<parameter name="silenceThreshold" required="false">
				<para>How long do we consider silence</para>
			</parameter>
			<parameter name="maximumWordLength" required="false">
				<para>Is the maximum duration of a word to accept.</para>
				<para>If exceeded set as MACHINE</para>
			</parameter>
		</syntax>
		<description>
			<para>This application attempts to detect automated dialers at the beginning
			of inbound calls. Simply call this application after the call
			has been answered. To prevent silence to the caller, execute a PlayTones(Ring).
			Just remember to StopPlayTones() right before you execute SPIT() or else you'll get interferance.</para>
			<para>When loaded, SPIT reads spit.conf and uses the parameters specified as
			default values. Those default values get overwritten when the calling SPIT
			with parameters.</para>
			<para>This application sets the following channel variables:</para>
			<variablelist>
				<variable name="SPITSTATUS">
					<para>This is the status of the answering machine detection</para>
					<value name="DIALER">
						Detected a stream of audio coming from the inbound caller.
					</value>
					<value name="HUMAN">
						There was some noise from the caller, but silence followed.
					</value>
					<value name="DTMF">
						A DTMF Frame was detected on the incoming channel, possible Phreaker
					</value>
					<value name="NOTSURE">
						Nothing conclusive before timeout, could be a bad connection or the caller munching on cheetos.
					</value>
					<value name="HANGUP" />
				</variable>
				<variable name="SPITCAUSE">
					<para>Indicates the cause that led to the conclusion</para>
					<value name="TIMEOUT">
						Total Analysis Time exceeded, possibly just background noise.
					</value>
					<value name="INITIALSILENCE">
						Silence Duration - Initial Silence.
					</value>
					<value name="SILENCEAFTERNOISE">
						Silence Duration - afterGreetingSilence, a cough maybe?.
					</value>
					<value name="RECORDING">
						Multiple words, probably an automated message.
					</value>
					<value name="MAXWORDLENGTH">
						Consistent noise beyond the max noise length.
					</value>
					<value name="MAXWORDS">
						Word Count - maximum number of words, probably an automated message.
					</value>	
					<value name="DTMFFRAME">
						AST_FRAME_DTMF_BEGIN or AST_FRAME_DTMF_END with digit.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="application">AMD</ref>
			<ref type="application">WaitForSilence</ref>
			<ref type="application">WaitForNoise</ref>
		</see-also>
	</application>

 ***/

static const char app[] = "SPIT";

#define STATE_IN_WORD       1
#define STATE_IN_SILENCE    2

/* Some default values for the algorithm parameters. These defaults will be overwritten from spit.conf */
static int dfltInitialSilence       = 2500;
static int dfltGreeting             = 1500;
static int dfltAfterGreetingSilence = 800;
static int dfltTotalAnalysisTime    = 5000;
static int dfltMinimumWordLength    = 100;
static int dfltBetweenWordsSilence  = 50;
static int dfltMaximumNumberOfWords = 3;
static int dfltSilenceThreshold     = 256;
static int dfltMaximumWordLength    = 5000; /* Setting this to a large default so it is not used unless specify it in the configs or command line */

/* Set to the lowest ms value provided in spit.conf or application parameters */
static int dfltMaxWaitTimeForFrame  = 50;

static void isAutomatedDialer(struct ast_channel *chan, const char *data)
{
	int res = 0;
	struct ast_frame *f = NULL;
	struct ast_dsp *silenceDetector = NULL;
	int dspsilence = 0, framelength = 0;
	RAII_VAR(struct ast_format *, readFormat, NULL, ao2_cleanup);
	int inInitialSilence = 1;
	int inGreeting = 0;
	int voiceDuration = 0;
	int silenceDuration = 0;
	int iTotalTime = 0;
	int iWordsCount = 0;
	int currentState = STATE_IN_WORD;
	int consecutiveVoiceDuration = 0;
	char spitCause[256] = "", spitStatus[256] = "";
	char *parse = ast_strdupa(data);

	/* Lets set the initial values of the variables that will control the algorithm.
	   The initial values are the default ones. If they are passed as arguments
	   when invoking the application, then the default values will be overwritten
	   by the ones passed as parameters. */
	int initialSilence       = dfltInitialSilence;
	int greeting             = dfltGreeting;
	int afterGreetingSilence = dfltAfterGreetingSilence;
	int totalAnalysisTime    = dfltTotalAnalysisTime;
	int minimumWordLength    = dfltMinimumWordLength;
	int betweenWordsSilence  = dfltBetweenWordsSilence;
	int maximumNumberOfWords = dfltMaximumNumberOfWords;
	int silenceThreshold     = dfltSilenceThreshold;
	int maximumWordLength    = dfltMaximumWordLength;
	int maxWaitTimeForFrame  = dfltMaxWaitTimeForFrame;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(argInitialSilence);
		AST_APP_ARG(argGreeting);
		AST_APP_ARG(argAfterGreetingSilence);
		AST_APP_ARG(argTotalAnalysisTime);
		AST_APP_ARG(argMinimumWordLength);
		AST_APP_ARG(argBetweenWordsSilence);
		AST_APP_ARG(argMaximumNumberOfWords);
		AST_APP_ARG(argSilenceThreshold);
		AST_APP_ARG(argMaximumWordLength);
	);

	ast_verb(3, "SPIT: %s %s %s (Fmt: %s)\n", ast_channel_name(chan),
		S_COR(ast_channel_caller(chan)->ani.number.valid, ast_channel_caller(chan)->ani.number.str, "(N/A)"),
		S_COR(ast_channel_redirecting(chan)->from.number.valid, ast_channel_redirecting(chan)->from.number.str, "(N/A)"),
		ast_format_get_name(ast_channel_readformat(chan)));

	/* Lets parse the arguments. */
	if (!ast_strlen_zero(parse)) {
		/* Some arguments have been passed. Lets parse them and overwrite the defaults. */
		AST_STANDARD_APP_ARGS(args, parse);
		if (!ast_strlen_zero(args.argInitialSilence))
			initialSilence = atoi(args.argInitialSilence);
		if (!ast_strlen_zero(args.argGreeting))
			greeting = atoi(args.argGreeting);
		if (!ast_strlen_zero(args.argAfterGreetingSilence))
			afterGreetingSilence = atoi(args.argAfterGreetingSilence);
		if (!ast_strlen_zero(args.argTotalAnalysisTime))
			totalAnalysisTime = atoi(args.argTotalAnalysisTime);
		if (!ast_strlen_zero(args.argMinimumWordLength))
			minimumWordLength = atoi(args.argMinimumWordLength);
		if (!ast_strlen_zero(args.argBetweenWordsSilence))
			betweenWordsSilence = atoi(args.argBetweenWordsSilence);
		if (!ast_strlen_zero(args.argMaximumNumberOfWords))
			maximumNumberOfWords = atoi(args.argMaximumNumberOfWords);
		if (!ast_strlen_zero(args.argSilenceThreshold))
			silenceThreshold = atoi(args.argSilenceThreshold);
		if (!ast_strlen_zero(args.argMaximumWordLength))
			maximumWordLength = atoi(args.argMaximumWordLength);
	} else {
		ast_debug(1, "SPIT using the default parameters.\n");
	}

	/* Find lowest ms value, that will be max wait time for a frame */
	if (maxWaitTimeForFrame > initialSilence)
		maxWaitTimeForFrame = initialSilence;
	if (maxWaitTimeForFrame > greeting)
		maxWaitTimeForFrame = greeting;
	if (maxWaitTimeForFrame > afterGreetingSilence)
		maxWaitTimeForFrame = afterGreetingSilence;
	if (maxWaitTimeForFrame > totalAnalysisTime)
		maxWaitTimeForFrame = totalAnalysisTime;
	if (maxWaitTimeForFrame > minimumWordLength)
		maxWaitTimeForFrame = minimumWordLength;
	if (maxWaitTimeForFrame > betweenWordsSilence)
		maxWaitTimeForFrame = betweenWordsSilence;

	/* Now we're ready to roll! */
	ast_verb(3, "SPIT: initialSilence [%d] greeting [%d] afterGreetingSilence [%d] "
		"totalAnalysisTime [%d] minimumWordLength [%d] betweenWordsSilence [%d] maximumNumberOfWords [%d] silenceThreshold [%d] maximumWordLength [%d] \n",
				initialSilence, greeting, afterGreetingSilence, totalAnalysisTime,
				minimumWordLength, betweenWordsSilence, maximumNumberOfWords, silenceThreshold, maximumWordLength);

	/* Set read format to signed linear so we get signed linear frames in */
	readFormat = ao2_bump(ast_channel_readformat(chan));
	if (ast_set_read_format(chan, ast_format_slin) < 0 ) {
		ast_log(LOG_WARNING, "SPIT: Channel [%s]. Unable to set to linear mode, giving up\n", ast_channel_name(chan));
		pbx_builtin_setvar_helper(chan , "SPITSTATUS", "NOTSLIN");
		pbx_builtin_setvar_helper(chan , "SPITCAUSE", "INVALIDFORMAT");
		return;
	}

	/* Create a new DSP that will detect the silence */
	if (!(silenceDetector = ast_dsp_new())) {
		ast_log(LOG_WARNING, "SPIT: Channel [%s]. Unable to create silence detector :(\n", ast_channel_name(chan));
		pbx_builtin_setvar_helper(chan , "SPITSTATUS", "NODETECTOR");
		pbx_builtin_setvar_helper(chan , "SPITCAUSE", "CANNOTCREATE");
		return;
	}

	/* Set silence threshold to specified value */
	ast_dsp_set_threshold(silenceDetector, silenceThreshold);

	/* Now we go into a loop waiting for frames from the channel */
	while ((res = ast_waitfor(chan, 2 * maxWaitTimeForFrame)) > -1) {

		/* If we fail to read in a frame, that means they hung up */
		if (!(f = ast_read(chan))) {
			ast_verb(3, "SPIT: Channel [%s]. HANGUP\n", ast_channel_name(chan));
			ast_debug(1, "Got hangup\n");
			strcpy(spitStatus, "HANGUP");
			res = 1;
			break;
		}

		if (f->frametype == AST_FRAME_DTMF_BEGIN || f->frametype == AST_FRAME_DTMF_END){
			ast_verb(3, "CPA: Channel [%s] has incoming DTMF, Digit received: [%d]\n", ast_channel_name(chan), f->subclass.integer);
			strcpy(spitStatus , "DTMF");
			sprintf(spitCause , "DTMFFRAME-%d", f->subclass.integer-48);
			res = 1;	
			break;
		}


		if (f->frametype == AST_FRAME_VOICE || f->frametype == AST_FRAME_NULL || f->frametype == AST_FRAME_CNG) {
			/* If the total time exceeds the analysis time then give up as we are not too sure */
			if (f->frametype == AST_FRAME_VOICE) {
				framelength = (ast_codec_samples_count(f) / DEFAULT_SAMPLES_PER_MS);
			} else {
				framelength = 2 * maxWaitTimeForFrame;
			}

			iTotalTime += framelength;
			if (iTotalTime >= totalAnalysisTime) {
				ast_verb(3, "SPIT: Channel [%s]. Nothing definitive before timeout...\n", ast_channel_name(chan));
				ast_frfree(f);
				strcpy(spitStatus , "NOTSURE");
				sprintf(spitCause , "TIMEOUT-%d", iTotalTime);
				break;
			}

			/* Feed the frame of audio into the silence detector and see if we get a result */
			if (f->frametype != AST_FRAME_VOICE)
				dspsilence += 2 * maxWaitTimeForFrame;
			else {
				dspsilence = 0;
				ast_dsp_silence(silenceDetector, f, &dspsilence);
			}

			if (dspsilence > 0) {
				silenceDuration = dspsilence;
				
				if (silenceDuration >= betweenWordsSilence) {
					if (currentState != STATE_IN_SILENCE ) {
						ast_verb(3, "SPIT: Channel [%s]. Changed state to STATE_IN_SILENCE\n", ast_channel_name(chan));
					}
					/* Find words less than word duration */
					if (consecutiveVoiceDuration < minimumWordLength && consecutiveVoiceDuration > 0){
						ast_verb(3, "SPIT: Channel [%s]. Short Word Duration: %d\n", ast_channel_name(chan), consecutiveVoiceDuration);
					}
					currentState  = STATE_IN_SILENCE;
					consecutiveVoiceDuration = 0;
				}

				if (inInitialSilence == 1  && silenceDuration >= initialSilence) {
					ast_verb(3, "SPIT: Channel [%s]. AUTOMATED DIALER: silenceDuration:%d initialSilence:%d\n",
						ast_channel_name(chan), silenceDuration, initialSilence);
					ast_frfree(f);
					strcpy(spitStatus , "HUMAN");
					sprintf(spitCause , "INITIALSILENCE-%d-%d", silenceDuration, initialSilence);
					res = 1;
					break;
				}
				
				if (silenceDuration >= afterGreetingSilence  &&  inGreeting == 1) {
					ast_verb(3, "SPIT: Channel [%s]. HUMAN: silenceDuration:%d afterGreetingSilence:%d\n",
						ast_channel_name(chan), silenceDuration, afterGreetingSilence);
					ast_frfree(f);
					strcpy(spitStatus , "HUMAN");
					sprintf(spitCause , "SILENCEAFTERNOISE-%d-%d", silenceDuration, afterGreetingSilence);
					res = 1;
					break;
				}
				
			} else {
				consecutiveVoiceDuration += framelength;
				voiceDuration += framelength;

				/* If I have enough consecutive voice to say that I am in a Word, I can only increment the
				   number of words if my previous state was Silence, which means that I moved into a word. */
				if (consecutiveVoiceDuration >= minimumWordLength && currentState == STATE_IN_SILENCE) {
					iWordsCount++;
					ast_verb(3, "SPIT: Channel [%s]. Word detected. iWordsCount:%d\n", ast_channel_name(chan), iWordsCount);
					currentState = STATE_IN_WORD;
				}
				if (consecutiveVoiceDuration >= maximumWordLength){
					ast_verb(3, "SPIT: Channel [%s]. Maximum Word Length detected. [%d]\n", ast_channel_name(chan), consecutiveVoiceDuration);
					ast_frfree(f);
					strcpy(spitStatus , "MACHINE");
					sprintf(spitCause , "MAXWORDLENGTH-%d", consecutiveVoiceDuration);
					break;
				}
				if (iWordsCount >= maximumNumberOfWords) {
					ast_verb(3, "SPIT: Channel [%s]. ANSWERING MACHINE: iWordsCount:%d\n", ast_channel_name(chan), iWordsCount);
					ast_frfree(f);
					strcpy(spitStatus , "MACHINE");
					sprintf(spitCause , "MAXWORDS-%d-%d", iWordsCount, maximumNumberOfWords);
					res = 1;
					break;
				}

				if (inGreeting == 1 && voiceDuration >= greeting) {
					ast_verb(3, "SPIT: Channel [%s]. ANSWERING MACHINE: voiceDuration:%d greeting:%d\n", ast_channel_name(chan), voiceDuration, greeting);
					ast_frfree(f);
					strcpy(spitStatus , "MACHINE");
					sprintf(spitCause , "LONGGREETING-%d-%d", voiceDuration, greeting);
					res = 1;
					break;
				}

				if (voiceDuration >= minimumWordLength ) {
					if (silenceDuration > 0)
						ast_verb(3, "SPIT: Channel [%s]. Detected Talk, previous silence duration: %d, current voice duration: %d\n", ast_channel_name(chan), silenceDuration, voiceDuration);
					silenceDuration = 0;
				}
				if (consecutiveVoiceDuration >= minimumWordLength && inGreeting == 0) {
					/* Only go in here once to change the greeting flag when we detect the 1st word */
					if (silenceDuration > 0)
						ast_verb(3, "SPIT: Channel [%s]. Before Greeting Time:  silenceDuration: %d voiceDuration: %d\n", ast_channel_name(chan), silenceDuration, voiceDuration);
					inInitialSilence = 0;
					inGreeting = 1;
				}
				
			}
			ast_debug(3, "SPIT: Channel [%s]: silenceDuration [%d] voiceDuration [%d] consecutiveVoiceDuration [%d]"
				" iWordsCount [%d] currentState [%d] inInitialSilence [%d] inGreeting [%d]\n",
				ast_channel_name(chan), silenceDuration, voiceDuration, consecutiveVoiceDuration,
				 iWordsCount, currentState, inInitialSilence, inGreeting);
		}
		ast_frfree(f);
	}
	
	if (!res) {
		/* It took too long to get a frame back. Giving up. */
		ast_verb(3, "SPIT: Channel [%s]. No frames detected...\n", ast_channel_name(chan));
		strcpy(spitStatus , "NOFRAMES");
		sprintf(spitCause , "TIMEOUT-%d", iTotalTime);
	}

	/* Set the status and cause on the channel */
	pbx_builtin_setvar_helper(chan , "SPITSTATUS" , spitStatus);
	pbx_builtin_setvar_helper(chan , "SPITCAUSE" , spitCause);

	/* Restore channel read format */
	if (readFormat && ast_set_read_format(chan, readFormat))
		ast_log(LOG_WARNING, "SPIT: Unable to restore read format on '%s'\n", ast_channel_name(chan));

	/* Free the DSP used to detect silence */
	ast_dsp_free(silenceDetector);

	return;
}


static int spit_exec(struct ast_channel *chan, const char *data)
{
	isAutomatedDialer(chan, data);

	return 0;
}

static int load_config(int reload)
{
	struct ast_config *cfg = NULL;
	char *cat = NULL;
	struct ast_variable *var = NULL;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	dfltSilenceThreshold = ast_dsp_get_threshold_from_settings(THRESHOLD_SILENCE);

	if (!(cfg = ast_config_load("spit.conf", config_flags))) {
		ast_log(LOG_ERROR, "Configuration file spit.conf missing.\n");
		return -1;
	} else if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file spit.conf is in an invalid format.  Aborting.\n");
		return -1;
	}

	cat = ast_category_browse(cfg, NULL);

	while (cat) {
		if (!strcasecmp(cat, "general") ) {
			var = ast_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "initial_silence")) {
					dfltInitialSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "greeting")) {
					dfltGreeting = atoi(var->value);
				} else if (!strcasecmp(var->name, "after_greeting_silence")) {
					dfltAfterGreetingSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "silence_threshold")) {
					dfltSilenceThreshold = atoi(var->value);
				} else if (!strcasecmp(var->name, "total_analysis_time")) {
					dfltTotalAnalysisTime = atoi(var->value);
				} else if (!strcasecmp(var->name, "min_word_length")) {
					dfltMinimumWordLength = atoi(var->value);
				} else if (!strcasecmp(var->name, "between_words_silence")) {
					dfltBetweenWordsSilence = atoi(var->value);
				} else if (!strcasecmp(var->name, "maximum_number_of_words")) {
					dfltMaximumNumberOfWords = atoi(var->value);
				} else if (!strcasecmp(var->name, "maximum_word_length")) {
					dfltMaximumWordLength = atoi(var->value);

				} else {
					ast_log(LOG_WARNING, "%s: Cat:%s. Unknown keyword %s at line %d of spit.conf\n",
						app, cat, var->name, var->lineno);
				}
				var = var->next;
			}
		}
		cat = ast_category_browse(cfg, cat);
	}

	ast_config_destroy(cfg);

	ast_verb(3, "SPIT defaults: initialSilence [%d] greeting [%d] afterGreetingSilence [%d] "
		"totalAnalysisTime [%d] minimumWordLength [%d] betweenWordsSilence [%d] maximumNumberOfWords [%d] silenceThreshold [%d] maximumWordLength [%d]\n",
		dfltInitialSilence, dfltGreeting, dfltAfterGreetingSilence, dfltTotalAnalysisTime,
		dfltMinimumWordLength, dfltBetweenWordsSilence, dfltMaximumNumberOfWords, dfltSilenceThreshold, dfltMaximumWordLength);

	return 0;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

/*!
 * \brief Load the module
 *
 * Module loading including tests for configuration or dependencies.
 * This function can return AST_MODULE_LOAD_FAILURE, AST_MODULE_LOAD_DECLINE,
 * or AST_MODULE_LOAD_SUCCESS. If a dependency or environment variable fails
 * tests return AST_MODULE_LOAD_FAILURE. If the module can not load the 
 * configuration file or other non-critical problem return 
 * AST_MODULE_LOAD_DECLINE. On success return AST_MODULE_LOAD_SUCCESS.
 */
static int load_module(void)
{
	if (load_config(0) || ast_register_application_xml(app, spit_exec)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (load_config(1))
		return AST_MODULE_LOAD_DECLINE;
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Automated Dialer Detection Application",
		.support_level = AST_MODULE_SUPPORT_EXTENDED,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
);
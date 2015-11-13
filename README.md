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
			Just remember to StopPlayTones() after you get your result.</para>
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

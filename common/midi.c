////////////////////////////////////////////////////////////////////////////////
// MIDI handling
////////////////////////////////////////////////////////////////////////////////

#include "midi.h"

#include "storage.h"
#include "ui.h"
#include "uart_6850.h"
#include "import.h"

#include "../xnormidi/midi_device.h"
#include "../xnormidi/midi.h"

#define MAX_SYSEX_SIZE TEMP_BUFFER_SIZE

#define MIDI_BASE_STEPPED_CC 48
#define MIDI_BASE_COARSE_CC 16
#define MIDI_BASE_FINE_CC 80
#define MIDI_BASE_NOTE 24

static MidiDevice midi;
static int16_t sysexSize;

extern void refreshFullState(void);
extern void refreshPresetMode(void);

static void sysexSend(uint8_t command, int16_t size)
{
	int16_t chunkCount,i;
	uint8_t chunk[4];
	
	BLOCK_INT
	{
		chunkCount=((size-1)>>2)+1;

		uart_send(0xf0);
		uart_send(SYSEX_ID_0);
		uart_send(SYSEX_ID_1);
		uart_send(SYSEX_ID_2);
		uart_send(command);

		for(i=0;i<chunkCount;++i)
		{
			memcpy(chunk,&tempBuffer[i<<2],4);

			uart_send(chunk[0]&0x7f);
			uart_send(chunk[1]&0x7f);
			uart_send(chunk[2]&0x7f);
			uart_send(chunk[3]&0x7f);
			uart_send(((chunk[0]>>7)&1) | ((chunk[1]>>6)&2) | ((chunk[2]>>5)&4) | ((chunk[3]>>4)&8));
		}

		uart_send(0xf7);
	}
}

static int16_t sysexDescrambleBuffer(int16_t start)
{
	int16_t chunkCount,i,out;
	uint8_t b;
	
	chunkCount=((sysexSize-start)/5)+1;
	out=start;

	for(i=0;i<chunkCount;++i)
	{
		memmove(&tempBuffer[out],&tempBuffer[i*5+start],4);
		
		b=tempBuffer[i*5+start+4];
		
		tempBuffer[out+0]|=(b&1)<<7;
		tempBuffer[out+1]|=(b&2)<<6;
		tempBuffer[out+2]|=(b&4)<<5;
		tempBuffer[out+3]|=(b&8)<<4;
		
		out+=4;
	}
	
	return out-start;
}

static void sysexReceiveByte(uint8_t b)
{
	int16_t size;

	switch(b)
	{
	case 0xF0:
		sysexSize=0;
		memset(tempBuffer,0,MAX_SYSEX_SIZE);
		break;
	case 0xF7:
		if(tempBuffer[0]==0x01 && tempBuffer[1]==0x02) // SCI P600 program dump
		{
			import_sysex(tempBuffer,sysexSize);
		}
		else if(tempBuffer[0]==SYSEX_ID_0 && tempBuffer[1]==SYSEX_ID_1 && tempBuffer[2]==SYSEX_ID_2) // my sysex ID
		{
			// handle my sysex commands
			
			switch(tempBuffer[3])
			{
			case SYSEX_COMMAND_BANK_A:
				size=sysexDescrambleBuffer(4);
				storage_import(tempBuffer[4],&tempBuffer[5],size-1);
				break;
			}
		}

		sysexSize=0;
		refreshFullState();
		break;
	default:
		if(sysexSize>=MAX_SYSEX_SIZE)
		{
#ifdef DEBUG
			print("Warning: sysex buffer overflow\n");
#endif
			sysexSize=0;
		}
		
		tempBuffer[sysexSize++]=b;
	}
}

static int8_t midiFilterChannel(uint8_t channel)
{
	return settings.midiReceiveChannel<0 || (channel&MIDI_CHANMASK)==settings.midiReceiveChannel;
}

static void midi_noteOnEvent(MidiDevice * device, uint8_t channel, uint8_t note, uint8_t velocity)
{
	int16_t intNote;
	
	if(!midiFilterChannel(channel))
		return;
	
#ifdef DEBUG_
	print("midi note on  ");
	phex(note);
	print("\n");
#endif

	intNote=note-MIDI_BASE_NOTE;
	intNote=MAX(0,intNote);
	
	assigner_assignNote(intNote,velocity!=0,((velocity+1)<<9)-1,0);
}

static void midi_noteOffEvent(MidiDevice * device, uint8_t channel, uint8_t note, uint8_t velocity)
{
	int16_t intNote;
	
	if(!midiFilterChannel(channel))
		return;
	
#ifdef DEBUG_
	print("midi note off ");
	phex(note);
	print("\n");
#endif

	intNote=note-MIDI_BASE_NOTE;
	intNote=MAX(0,intNote);
	
	assigner_assignNote(intNote,0,0,0);
}

static void midi_ccEvent(MidiDevice * device, uint8_t channel, uint8_t control, uint8_t value)
{
	int16_t param;
	
	if(!midiFilterChannel(channel))
		return;
	
#ifdef DEBUG_
	print("midi cc ");
	phex(control);
	print(" value ");
	phex(value);
	print("\n");
#endif

	if(control==0 && value<=1 && settings.presetMode!=value) // coarse bank #
	{
		settings.presetMode=value;
		settings_save();
		refreshPresetMode();
		refreshFullState();
	}
	
	if(!settings.presetMode) // in manual mode CC changes would only conflict with pot scans...
		return;
	
	if(control>=MIDI_BASE_COARSE_CC && control<MIDI_BASE_COARSE_CC+cpCount)
	{
		param=control-MIDI_BASE_COARSE_CC;

		currentPreset.continuousParameters[param]&=0x01fc;
		currentPreset.continuousParameters[param]|=(uint16_t)value<<9;
		ui.presetModified=1;	
	}
	else if(control>=MIDI_BASE_FINE_CC && control<MIDI_BASE_FINE_CC+cpCount)
	{
		param=control-MIDI_BASE_FINE_CC;

		currentPreset.continuousParameters[param]&=0xfe00;
		currentPreset.continuousParameters[param]|=(uint16_t)value<<2;
		ui.presetModified=1;	
	}
	else if(control>=MIDI_BASE_STEPPED_CC && control<MIDI_BASE_STEPPED_CC+spCount)
	{
		param=control-MIDI_BASE_STEPPED_CC;
		
		currentPreset.steppedParameters[param]=value>>(7-steppedParametersBits[param]);
		ui.presetModified=1;	
	}

	if(ui.presetModified)
		refreshFullState();
}

static void midi_progChangeEvent(MidiDevice * device, uint8_t channel, uint8_t program)
{
	if(!midiFilterChannel(channel))
		return;

	if(settings.presetMode && program<100  && program!=settings.presetNumber)
	{
		if(preset_loadCurrent(program))
		{
			settings.presetNumber=program;
			ui.lastActivePot=ppNone;
			ui.presetModified=0;
			settings_save();		
			refreshFullState();
		}
	}
}

static void midi_sysexEvent(MidiDevice * device, uint16_t count, uint8_t b0, uint8_t b1, uint8_t b2)
{
	if(sysexSize)
		count=count-sysexSize-1;
	
	if(count>0)
		sysexReceiveByte(b0);
	
	if(count>1)
		sysexReceiveByte(b1);

	if(count>2)
		sysexReceiveByte(b2);
}

void midi_init(void)
{
	midi_device_init(&midi);
	midi_register_noteon_callback(&midi,midi_noteOnEvent);
	midi_register_noteoff_callback(&midi,midi_noteOffEvent);
	midi_register_cc_callback(&midi,midi_ccEvent);
	midi_register_progchange_callback(&midi,midi_progChangeEvent);
	midi_register_sysex_callback(&midi,midi_sysexEvent);
	
	sysexSize=0;
}

void midi_update(void)
{
	midi_device_process(&midi);
}

void midi_newData(uint8_t data)
{
	midi_device_input(&midi,1,&data);
}

void midi_dumpPresets(void)
{
	int8_t i;
	int16_t size=0;

	for(i=0;i<100;++i)
	{
		if(preset_loadCurrent(i))
		{
			storage_export(i,tempBuffer,&size);
			sysexSend(SYSEX_COMMAND_BANK_A,size);
		}
	}
}

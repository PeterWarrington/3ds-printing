#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>

#include <3ds.h>
#include "pdfgen.h"
#include "raster.h"
#include "font8x8_basic.h"
#include <sys/socket.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define IPP_VERSION 0x0101 
#define IPP_PRINT_JOB 0x0002
#define USER_AGENT "ipp-3ds/1.0.0"
#define IPP_CONTENT_TYPE "application/ipp"
#define IPP_OPERATION_ATTRIBUTES_TAG 0x01
#define IPP_MIME_MEDIA_TYPE_VALUE_TAG 0x49
#define IPP_CHARSET_VALUE_TAG 0x47
#define IPP_CHARSET_NATURAL_LANGUAGE 0x48
#define IPP_URI_VALUE_TAG 0x45
#define IPP_END_OF_ATTRIBUTES 0x03
#define TYPE_PDF 1
#define TYPE_TEXT 2
#define TYPE_PWG_RASTER 0
#define PROTOCOL_IPP 0
#define PROTOCOL_9100 1
#define PAGE_WIDTH 595
#define PAGE_HEIGHT 842

/* 
	The cups_page_header2_s struct is sourced from https://github.com/OpenPrinting/cups/blob/master/cups/raster.h
	Part of the CUPS Imaging library.
	Copyright © 2007-2018 by Apple Inc.
	Copyright © 1997-2006 by Easy Software Products.
	Licensed under Apache License v2.0.
*/
struct ipp_payload_header;

struct ipp_payload_header
{
	int16_t versionNumber; // 2,0
	int16_t operationId; // one of those in https://www.rfc-editor.org/rfc/rfc8011.html#section-5.4.15
	int32_t requestId; // Unique to each request
	char attributeGroups [];
};

struct ipp_payload_body
{
	int8_t attributesEnd;
	char data[];
};

char *get_input(char* prompt, char* initial) {
	SwkbdState swkbd;
	char *mybuf = malloc(60);

	swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, 60);
	swkbdSetInitialText(&swkbd, initial);
	swkbdSetHintText(&swkbd, prompt);
	swkbdSetButton(&swkbd, SWKBD_BUTTON_CONFIRM, "Submit", true);
	swkbdInputText(&swkbd, mybuf, 60);

	return mybuf;
}

// This function based on https://github.com/devkitPro/3ds-examples/blob/master/network/http_post/source/main.c
Result ipp_post(const char* url, u32 *data, u32 payloadSize)
{
	Result ret=0;
	httpcContext context;
	char *newurl=NULL;
	u32 statuscode=0;
	u32 contentsize=0, readsize=0, size=0;
	u8 *buf, *lastbuf;

	printf("POSTing %s\n", url);

	do {
		ret = httpcOpenContext(&context, HTTPC_METHOD_POST, url, 0);

		// This disables SSL cert verification, so https:// will be usable
		ret = httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);

		// Enable Keep-Alive connections
		ret = httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_ENABLED);

		// Set a User-Agent header so websites can identify your application
		ret = httpcAddRequestHeaderField(&context, "User-Agent", USER_AGENT);

		// Set a Content-Type header so websites can identify the format of our raw body data.
		// If you want to send form data in your request, use:
		//ret = httpcAddRequestHeaderField(&context, "Content-Type", "multipart/form-data");
		// If you want to send raw JSON data in your request, use:
		ret = httpcAddRequestHeaderField(&context, "Content-Type", IPP_CONTENT_TYPE);

		// Post specified data.
		// If you want to add a form field to your request, use:
		//ret = httpcAddPostDataAscii(&context, "data", value);
		// If you want to add a form field containing binary data to your request, use:
		//ret = httpcAddPostDataBinary(&context, "field name", yourBinaryData, length);
		// If you want to add raw data to your request, use:
		ret = httpcAddPostDataRaw(&context, data, payloadSize);

		ret = httpcBeginRequest(&context);
		if(ret!=0){
			httpcCloseContext(&context);
			if(newurl!=NULL) free(newurl);
			return ret;
		}

		ret = httpcGetResponseStatusCode(&context, &statuscode);
		if(ret!=0){
			httpcCloseContext(&context);
			if(newurl!=NULL) free(newurl);
			return ret;
		}

		if ((statuscode >= 301 && statuscode <= 303) || (statuscode >= 307 && statuscode <= 308)) {
			if(newurl==NULL) newurl = malloc(0x1000); // One 4K page for new URL
			if (newurl==NULL){
				httpcCloseContext(&context);
				return -1;
			}
			ret = httpcGetResponseHeader(&context, "Location", newurl, 0x1000);
			url = newurl; // Change pointer to the url that we just learned
			printf("redirecting to url: %s\n",url);
			httpcCloseContext(&context); // Close this context before we try the next
		}
	} while ((statuscode >= 301 && statuscode <= 303) || (statuscode >= 307 && statuscode <= 308));

	if(statuscode!=200){
		printf("URL returned status: %" PRIx32 "\n", statuscode);
		// httpcCloseContext(&context);
		// if(newurl!=NULL) free(newurl);
		// return -2;
	}

	// This relies on an optional Content-Length header and may be 0
	ret=httpcGetDownloadSizeState(&context, NULL, &contentsize);
	if(ret!=0){
		httpcCloseContext(&context);
		if(newurl!=NULL) free(newurl);
		return ret;
	}

	// Start with a single page buffer
	buf = (u8*)malloc(0x1000);
	if(buf==NULL){
		httpcCloseContext(&context);
		if(newurl!=NULL) free(newurl);
		return -1;
	}

	do {
		// This download loop resizes the buffer as data is read.
		ret = httpcDownloadData(&context, buf+size, 0x1000, &readsize);
		size += readsize; 
		if (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING){
				lastbuf = buf; // Save the old pointer, in case realloc() fails.
				buf = realloc(buf, size + 0x1000);
				if(buf==NULL){ 
					httpcCloseContext(&context);
					free(lastbuf);
					if(newurl!=NULL) free(newurl);
					return -1;
				}
			}
	} while (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING);	

	if(ret!=0){
		httpcCloseContext(&context);
		if(newurl!=NULL) free(newurl);
		free(buf);
		return -1;
	}

	// Resize the buffer back down to our actual final size
	lastbuf = buf;
	buf = realloc(buf, size);
	if(buf==NULL){ // realloc() failed.
		httpcCloseContext(&context);
		free(lastbuf);
		if(newurl!=NULL) free(newurl);
		return -1;
	}

	printf("response size: %" PRIx32 "\n",size);

	// Print result
	// printf((char*)buf);
	// printf("\n");
	
	gfxFlushBuffers();
	gfxSwapBuffers();

	httpcCloseContext(&context);
	free(buf);
	if (newurl!=NULL) free(newurl);

	return 0;
}

int main()
{
	Result ret=0;
	gfxInitDefault();
	httpcInit(4 * 1024 * 1024); // Buffer size when POST/PUT.

	consoleInit(GFX_BOTTOM,NULL);

	printf("Welcome to Printing for 3DS!\n\n");

	printf("First, enter the Printer IP/Domain.\nPress A to continue.\n\n");
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_A)
			break;
	}

	char *printer_ip = get_input("Printer IP/Domain:", "192.168.");

	printf("You will now enter the document text.\nPress A to continue.\n\n");
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_A)
			break;
	}

	char *doc_text = get_input("Document text:", "This is printing from a 3DS!!");
	char *document_format;

	int body_size;
	char* body_data;

	char protocol_choices[2][50] = {"Internet Printing Protocol", "9100 port (plain text) printing"};
	int protocol_choice = 0;

	printf("Press A to select\n'%s'\nas protocol mode.\nPress -> to see next option.\n\n", protocol_choices[protocol_choice]);

	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_DRIGHT) {
			protocol_choice = (protocol_choice + 1) % 2;
			printf("Press A to select\n'%s'\nas protocol mode.\nPress -> to see next option.\n\n", protocol_choices[protocol_choice]);
		}
		if (kDown & KEY_A)
			break;
	}

	if (protocol_choice == PROTOCOL_IPP) {
		char format_choices[3][50] = {"PWG Raster", "PDF", "Plain text"};
		int format_choice = 0;

		printf("Now, select a document format.\nNot all printers support all formats.\n");
		printf("Press A to select\n'%s' as document format.\nPress -> to see next option.\n\n", format_choices[format_choice]);

		while (aptMainLoop())
		{
			gspWaitForVBlank();
			hidScanInput();

			u32 kDown = hidKeysDown();
			if (kDown & KEY_A)
				break; // break in order to return to hbmenu
			if (kDown & KEY_DRIGHT) {
				format_choice = (format_choice + 1) % 3;

				printf("Press A to select\n'%s' as document format.\nPress -> to see next option.\n\n", format_choices[format_choice]);
			}
		}
		
		printf("\n");

		if (format_choice == TYPE_PDF) {
			char *document_format_temp = "application/pdf";
			document_format = document_format_temp;

			struct pdf_doc *pdf = pdf_create(PDF_A4_WIDTH, PDF_A4_HEIGHT, NULL);
			pdf_append_page(pdf);
			pdf_add_text(pdf, NULL, doc_text, 12, 50, 20, PDF_BLACK);
			pdf_save(pdf, ".temp_pdf.pdf");
			pdf_destroy(pdf);

			// Get pdf size
			FILE *pdf_file = fopen(".temp_pdf.pdf", "r");

			fseek(pdf_file, 0L, SEEK_END);
			body_size = ftell(pdf_file);

			// Create body buffer
			body_data = malloc(body_size);
			fseek(pdf_file, 0L, SEEK_SET);
			fread(body_data, body_size, 1, pdf_file);

			remove(".temp_pdf.pdf");
		}
		if (format_choice == TYPE_TEXT) {
			char *document_format_temp = "text/plain";
			document_format = document_format_temp;

			body_data = doc_text;
			body_size = strlen(doc_text);
		}
		if (format_choice == TYPE_PWG_RASTER) {
			char *document_format_temp = "image/pwg-raster";
			document_format = document_format_temp;

			cups_page_header_t pwg_header = {
				.MediaClass = "PwgRaster",
				.MediaColor = "",
				.MediaType = "",
				.OutputType = "",
				.AdvanceDistance = 0,
				.Collate = 0,
				.CutMedia = 0,
				.Duplex = 0,
				.HWResolution = {64, 64},
				.ImagingBoundingBox = {64, 64, 64, 64},
				.InsertSheet = 0,
				.Jog = 0,
				.LeadingEdge = 0,
				.Margins = {64, 64},
				.ManualFeed = 0,
				.MediaPosition = 0,
				.MediaWeight = 0,
				.MirrorPrint = 0,
				.NegativePrint = 0,
				.NumCopies = 0,
				.Orientation = 0,
				.OutputFaceUp = 0,
				.PageSize = {PAGE_WIDTH, PAGE_HEIGHT},
				.Separations = 0,
				.TraySwitch = 0,
				.Tumble = 0,
				.cupsWidth = 595,
				.cupsHeight = 842,
				.cupsMediaType = 0,
				.cupsBitsPerColor = 1,
				.cupsBitsPerPixel = 1,
				.cupsBytesPerLine = 1,
				.cupsColorOrder = 0,
				.cupsColorSpace = 3,
				.cupsCompression = 0,
				.cupsRowCount = 0,
				.cupsRowFeed = 0,
				.cupsRowStep = 0
			};

			struct fullRaster {
				char syncWord[4];
				cups_page_header_t header;
				char data[2000];
			};

			struct fullRaster pwg_body_data;
			strcpy(pwg_body_data.syncWord, "tSaR");
			pwg_body_data.header = pwg_header;

			int rasterBodyOffset = 0;
			int textLen = strlen(doc_text);

			for (int y=0; y<8; y++) {
				for (int i=0; i<textLen; i++) {
					int ascii_code = (int) doc_text[i];
					pwg_body_data.data[rasterBodyOffset] = font8x8_basic[ascii_code][y];
					rasterBodyOffset++;
				}
				int remainingPoints = PAGE_WIDTH - (8*textLen);
				for (int i=0; i<remainingPoints; i++) {
					pwg_body_data.data[rasterBodyOffset] = 0;
					rasterBodyOffset++;
				}
			}

			body_size = sizeof(pwg_body_data);
			body_data = malloc(body_size);
			memcpy(body_data, &pwg_body_data, body_size);
		}

		char *attribute_names[] = {"document-format", "attributes-charset", "attributes-natural-language", "printer-uri"};
		int8_t attribute_value_tags[] = {IPP_MIME_MEDIA_TYPE_VALUE_TAG, IPP_CHARSET_VALUE_TAG, IPP_CHARSET_NATURAL_LANGUAGE, IPP_URI_VALUE_TAG};
		char *ipp_uri = malloc(500);
		sprintf(ipp_uri, "ipp://%s/ipp/print", printer_ip);
		char *attribute_values[] = {document_format, "utf-8", "en", ipp_uri};

		int http_payload_size = 1 + 1 + 8 + 8 + body_size;
		for (int i = 0; i < 4; i++)
		{
			http_payload_size += 1 + 2 + 2;
			http_payload_size += strlen(attribute_names[i]);
			http_payload_size += strlen(attribute_values[i]);
		}
		printf("attribute size title %d\n", http_payload_size);

		char *http_payload = malloc(http_payload_size);
		int http_payload_offset = 0;

		// Write IPP version
		int16_t ipp_version = htons(IPP_VERSION);
		memcpy(&http_payload[http_payload_offset], &ipp_version, 2);
		http_payload_offset += 2;

		// Write operation id
		int16_t ipp_print_job = htons(IPP_PRINT_JOB);
		memcpy(&http_payload[http_payload_offset], &ipp_print_job, 2);
		http_payload_offset += 2;

		// Write request ID
		int32_t ipp_request_id = htons(500);
		memcpy(&http_payload[http_payload_offset], &ipp_request_id, 4);
		http_payload_offset += 4;

		// Write operation attributes tag
		http_payload[http_payload_offset] = IPP_OPERATION_ATTRIBUTES_TAG;
		http_payload_offset += 1;

		for (int i = 0; i < 4; i++)
		{
			// Write value tag
			http_payload[http_payload_offset] = attribute_value_tags[i];
			http_payload_offset += 1;

			// Write name length
			int16_t name_length = htons(strlen(attribute_names[i]));
			memcpy(&http_payload[http_payload_offset], &name_length, 2);
			http_payload_offset += 2;

			// Write name
			memcpy(&http_payload[http_payload_offset], attribute_names[i], strlen(attribute_names[i]));
			http_payload_offset += strlen(attribute_names[i]);

			// Write value length
			int16_t value_length = htons(strlen(attribute_values[i]));
			memcpy(&http_payload[http_payload_offset], &value_length, 2);
			http_payload_offset += 2;

			// Write value
			memcpy(&http_payload[http_payload_offset], attribute_values[i], strlen(attribute_values[i]));
			http_payload_offset += strlen(attribute_values[i]);
		}

		http_payload[http_payload_offset] =  IPP_END_OF_ATTRIBUTES;
		http_payload_offset += 1;

		memcpy(&http_payload[http_payload_offset], body_data, body_size);
		http_payload_offset += body_size;

		printf("data size created: %u\n", http_payload_offset);
		char *printer_http_addr = malloc(500);
		sprintf(printer_http_addr, "http://%s:631/ipp/print", printer_ip);
		ret = ipp_post(printer_http_addr, (u32*) http_payload, http_payload_offset);
		printf("return from http_post: %" PRIx32 "\n",ret);
	}

	if (protocol_choice == PROTOCOL_9100) {
		printf("Beginning 9100 protocol send...\n");

		static u32 *SOC_buffer = NULL;

		SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

		if(SOC_buffer == NULL) {
			printf("memalign: failed to allocate\n");
		}

		// Now intialise soc:u service
		if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0) {
			printf("socInit: 0x%08X\n", (unsigned int)ret);
		}

		int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (sockfd < 0) {
			printf("socket: %d %s\n", errno, strerror(errno));
		}

		struct sockaddr_in destAddr;
		destAddr.sin_family = AF_INET;
		destAddr.sin_port = htons(9100);
		inet_pton(AF_INET, printer_ip, &destAddr.sin_addr);

		if (connect(sockfd, (struct sockaddr*)&destAddr, sizeof(destAddr)) < 0) {
			// Handle connection error
			printf("Connection failed.\n");
		}

		// set client socket to blocking to simplify sending data back
		printf("Sending...\n");
		char* pcl_command = malloc(500);
		sprintf(pcl_command, "\033%%-12345X@PJL\n@PJL ENTER LANGUAGE = PCL\n\033E%s\033*s-257X%%-12345X", printer_ip);
		int bytesSent = send(sockfd, pcl_command, strlen(pcl_command), 0);

		if (bytesSent < 0) {
			// Handle error
			printf("Error sending data.\n");
		}

		printf("Done!\n");
	}

	printf("\nOperation completed.\nPress START to exit app, or A to start again\nwith different options.\n");

	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		// Your code goes here

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
		if (kDown & KEY_A)
			return main();
		
		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();
	}

	// Exit services
	httpcExit();
	gfxExit();
	return 0;
}


/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  OpenGIS Web Coverage Server (WCS) 1.1.0 Implementation.  This
 *           file holds some WCS 1.1.0 specific functions but other parts
 *           are still implemented in mapwcs.c.
 * Author:   Frank Warmerdam and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 2007, Frank Warmerdam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in 
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapserver.h"
#include "maperror.h"
#include "mapthread.h"

MS_CVSID("$Id: mapwcs.c 6637 2007-08-17 21:30:26Z warmerdam $")

#if defined(USE_WCS_SVR) && defined(USE_LIBXML2)

#include "mapwcs.h"
#include "maplibxml2.h"
#include "gdal.h"
#include "cpl_string.h" /* GDAL string handling */

/************************************************************************/
/*                       msWCSGetFormatsList11()                        */
/*                                                                      */
/*      Fetch back a comma delimited formats list for the past layer    */
/*      if one is supplied, otherwise for all formats supported by      */
/*      the server.  Formats should be identified by mime type.         */
/************************************************************************/

static char *msWCSGetFormatsList11( mapObj *map, layerObj *layer )

{
    char *format_list = strdup("");
    char **tokens = NULL, **formats = NULL;
    int  i, numtokens = 0, numformats;
    const char *value;

/* -------------------------------------------------------------------- */
/*      Parse from layer metadata.                                      */
/* -------------------------------------------------------------------- */
    if( layer != NULL 
        && (value = msOWSGetEncodeMetadata( &(layer->metadata),"COM","formats",
                                            "GTiff" )) != NULL ) {
        tokens = msStringSplit(value, ' ', &numtokens);
    }

/* -------------------------------------------------------------------- */
/*      Or generate from all configured raster output formats that      */
/*      look plausible.                                                 */
/* -------------------------------------------------------------------- */
    else
    {
        tokens = (char **) calloc(map->numoutputformats,sizeof(char*));
        for( i = 0; i < map->numoutputformats; i++ )
        {
            switch( map->outputformatlist[i]->renderer )
            {
                /* seeminly normal raster format */
              case MS_RENDER_WITH_GD:
              case MS_RENDER_WITH_AGG:
              case MS_RENDER_WITH_RAWDATA:
                tokens[numtokens++] = strdup(map->outputformatlist[i]->name);
                break;
                
                /* rest of formats aren't really WCS compatible */
              default:
                break;
                
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Convert outputFormatObj names into mime types and remove        */
/*      duplicates.                                                     */
/* -------------------------------------------------------------------- */
    numformats = 0;
    formats = (char **) calloc(sizeof(char*),numtokens);
    
    for( i = 0; i < numtokens; i++ )
    {
        int format_i, j;
        const char *mimetype;
        
        for( format_i = 0; format_i < map->numoutputformats; format_i++ )
        {
            if( strcasecmp(map->outputformatlist[format_i]->name,
                           tokens[i]) == 0 )
                break;
        }
        

        if( format_i == map->numoutputformats )
        {
            msDebug("Failed to find outputformat info on format '%s', ignore.",
                    tokens[i] );
            continue;
        }

        mimetype = map->outputformatlist[format_i]->mimetype;
        if( mimetype == NULL || strlen(mimetype) == 0 )
        {
            msDebug("No mimetime for format '%s', ignoring.",
                    tokens[i] );
            continue;
        }
        
        for( j = 0; j < numformats; j++ )
        {
            if( strcasecmp(mimetype,formats[j]) == 0 )
                break;
        }

        if( j < numformats )
        {
            msDebug( "Format '%s' ignored since mimetype '%s' duplicates another outputFormatObj.", 
                     tokens[i], mimetype );
            continue;
        }
        
        formats[numformats++] = strdup(mimetype);
    }

    msFreeCharArray(tokens,numtokens);

/* -------------------------------------------------------------------- */
/*      Turn mimetype list into comma delimited form for easy use       */
/*      with xml functions.                                             */
/* -------------------------------------------------------------------- */
    for(i=0; i<numformats; i++) 
    {
        int       new_length;
        const char *format = formats[i];
            
        new_length = strlen(format_list) + strlen(format) + 2;
        format_list = (char *) realloc(format_list,new_length);
        
        if( strlen(format_list) > 0 )
            strcat( format_list, "," );
        strcat( format_list, format );
    }
    msFreeCharArray(formats,numformats);

    return format_list;
}

/************************************************************************/
/*                msWCSGetCapabilities11_CoverageSummary()              */
/*                                                                      */
/*      Generate a WCS 1.1 CoverageSummary.                             */
/************************************************************************/

static int msWCSGetCapabilities11_CoverageSummary(
    mapObj *map, wcsParamsObj *params, cgiRequestObj *req, 
    xmlDocPtr doc, xmlNodePtr psContents, layerObj *layer )

{
    coverageMetadataObj cm;
    int status;
    const char *value;
    char *owned_value;
    char *format_list;
    xmlNodePtr psCSummary;
    xmlNsPtr psOwsNs = xmlSearchNs( doc, psContents, BAD_CAST "ows" );
    char **tokens = NULL;
    int i = 0;
    int n = 0;

    status = msWCSGetCoverageMetadata(layer, &cm);
    if(status != MS_SUCCESS) return MS_FAILURE;

    psCSummary = xmlNewChild( psContents, NULL, "CoverageSummary", NULL );

/* -------------------------------------------------------------------- */
/*      Title (from description)                                        */
/* -------------------------------------------------------------------- */
    value = msOWSLookupMetadata( &(layer->metadata), "COM", "description");
    if( value == NULL )
        value = layer->name;
    xmlNewChild( psCSummary, psOwsNs, "Title", value );

/* -------------------------------------------------------------------- */
/*      Identifier (layer name)                                         */
/* -------------------------------------------------------------------- */
    xmlNewChild( psCSummary, NULL, "Identifier", layer->name );

/* -------------------------------------------------------------------- */
/*      Keywords                                                        */
/* -------------------------------------------------------------------- */
    value = msOWSLookupMetadata(&(layer->metadata), "COM", "keywordlist");

    if (value) {
        xmlNodePtr psNode;

        psNode = xmlNewChild(psCSummary, psOwsNs, "Keywords", NULL);

        tokens = msStringSplit(value, ',', &n);
        if (tokens && n > 0) {
            for (i=0; i<n; i++) {
                xmlNewChild(psNode, NULL, "Keyword", tokens[i] );
            }
            msFreeCharArray(tokens, n);
        }
    }

/* -------------------------------------------------------------------- */
/*      imageCRS bounding box.                                          */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psCSummary,
        msOWSCommonBoundingBox( psOwsNs, "urn:ogc:def:crs:OGC::imageCRS",
                                2, 0, 0, cm.xsize-1, cm.ysize-1 ));

/* -------------------------------------------------------------------- */
/*      native CRS bounding box.                                        */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psCSummary,
        msOWSCommonBoundingBox( psOwsNs, cm.srs_urn, 2, 
                                cm.extent.minx, cm.extent.miny,
                                cm.extent.maxx, cm.extent.maxy ));

/* -------------------------------------------------------------------- */
/*      WGS84 bounding box.                                             */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psCSummary,
        msOWSCommonWGS84BoundingBox( psOwsNs, 2,
                                     cm.llextent.minx, cm.llextent.miny,
                                     cm.llextent.maxx, cm.llextent.maxy ));

/* -------------------------------------------------------------------- */
/*      SupportedFormats                                                */
/* -------------------------------------------------------------------- */
    format_list = msWCSGetFormatsList11( map, layer );

    if (strlen(format_list) > 0 )
        msLibXml2GenerateList( psCSummary, NULL, "SupportedFormat", 
                                format_list, ',' );

    msFree( format_list );
    
/* -------------------------------------------------------------------- */
/*      Supported CRSes.                                                */
/* -------------------------------------------------------------------- */
    if( (owned_value = 
         msOWSGetProjURN( &(layer->projection), &(layer->metadata), 
                          "COM", MS_FALSE)) != NULL ) {
        /* ok */
    } else if((owned_value = 
               msOWSGetProjURN( &(layer->map->projection), 
                                &(layer->map->web.metadata), 
                                "COM", MS_FALSE)) != NULL ) {
        /* ok */
    } else 
        msDebug( "mapwcs.c: missing required information, no SRSs defined.");
    
    if( owned_value != NULL && strlen(owned_value) > 0 ) 
        msLibXml2GenerateList( psCSummary, NULL, "SupportedCRS", 
                                owned_value, ' ' );

    msFree( owned_value );
  
    return MS_SUCCESS;
}

/************************************************************************/
/*                       msWCSGetCapabilities11()                       */
/************************************************************************/
int msWCSGetCapabilities11(mapObj *map, wcsParamsObj *params, 
                                  cgiRequestObj *req)
{
    xmlDocPtr psDoc = NULL;       /* document pointer */
    xmlNodePtr psRootNode, psMainNode, psNode;
    xmlNodePtr psTmpNode;
    char *identifier_list = NULL, *format_list = NULL;
    xmlNsPtr psOwsNs, psXLinkNs;

    char *script_url=NULL, *script_url_encoded=NULL;

    xmlChar *buffer = NULL;
    int size = 0, i;
    msIOContext *context = NULL;

/* -------------------------------------------------------------------- */
/*      Build list of layer identifiers available.                      */
/* -------------------------------------------------------------------- */
    identifier_list = strdup("");
    for(i=0; i<map->numlayers; i++)
    {
        layerObj *layer = map->layers[i];
        int       new_length;

        if(!msWCSIsLayerSupported(layer)) 
            continue;

        new_length = strlen(identifier_list) + strlen(layer->name) + 2;
        identifier_list = (char *) realloc(identifier_list,new_length);

        if( strlen(identifier_list) > 0 )
            strcat( identifier_list, "," );
        strcat( identifier_list, layer->name );
    }

/* -------------------------------------------------------------------- */
/*      Create document.                                                */
/* -------------------------------------------------------------------- */
    psDoc = xmlNewDoc("1.0");

    psRootNode = xmlNewNode(NULL, "Capabilities");

    xmlDocSetRootElement(psDoc, psRootNode);

/* -------------------------------------------------------------------- */
/*      Name spaces                                                     */
/* -------------------------------------------------------------------- */
    xmlSetNs(psRootNode, xmlNewNs(psRootNode, "http://www.opengis.net/wcs/1.1", NULL));
    psOwsNs = xmlNewNs(psRootNode, MS_OWSCOMMON_OWS_NAMESPACE_URI, MS_OWSCOMMON_OWS_NAMESPACE_PREFIX);
    psXLinkNs = xmlNewNs(psRootNode, MS_OWSCOMMON_W3C_XLINK_NAMESPACE_URI, MS_OWSCOMMON_W3C_XLINK_NAMESPACE_PREFIX);
    xmlNewNs(psRootNode, MS_OWSCOMMON_W3C_XSI_NAMESPACE_URI, MS_OWSCOMMON_W3C_XSI_NAMESPACE_PREFIX);
    xmlNewNs(psRootNode, MS_OWSCOMMON_OGC_NAMESPACE_URI, MS_OWSCOMMON_OGC_NAMESPACE_PREFIX );

    xmlNewProp(psRootNode, "version", params->version );

/* -------------------------------------------------------------------- */
/*      Service metadata.                                               */
/* -------------------------------------------------------------------- */

    psTmpNode = xmlAddChild(psRootNode, msOWSCommonServiceIdentification(
                                psOwsNs, map, "OGC WCS", params->version));

    /*service provider*/
    psTmpNode = xmlAddChild(psRootNode, msOWSCommonServiceProvider(
                                psOwsNs, psXLinkNs, map));

    /*operation metadata */
    if ((script_url=msOWSGetOnlineResource(map, "COM", "onlineresource", req)) == NULL 
        || (script_url_encoded = msEncodeHTMLEntities(script_url)) == NULL)
    {
        return msWCSException(map, params->version, 
                              "NoApplicableCode", "NoApplicableCode");
    }
    free( script_url );

/* -------------------------------------------------------------------- */
/*      Operations metadata.                                            */
/* -------------------------------------------------------------------- */
    psMainNode= xmlAddChild(psRootNode,msOWSCommonOperationsMetadata(psOwsNs));

/* -------------------------------------------------------------------- */
/*      GetCapabilities - add Sections and AcceptVersions?              */
/* -------------------------------------------------------------------- */
    psNode = msOWSCommonOperationsMetadataOperation( 
        psOwsNs, psXLinkNs,
        "GetCapabilities", OWS_METHOD_GET, script_url_encoded);

    xmlAddChild(psMainNode, psNode);
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "service", "WCS"));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "version", (char *)params->version));

/* -------------------------------------------------------------------- */
/*      DescribeCoverage                                                */
/* -------------------------------------------------------------------- */
    psNode = msOWSCommonOperationsMetadataOperation(
        psOwsNs, psXLinkNs,
        "DescribeCoverage", OWS_METHOD_GET, script_url_encoded);

    xmlAddChild(psMainNode, psNode);
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "service", "WCS"));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "version", (char *)params->version));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "identifiers", identifier_list ));

/* -------------------------------------------------------------------- */
/*      GetCoverage                                                     */
/* -------------------------------------------------------------------- */
    psNode = msOWSCommonOperationsMetadataOperation(
        psOwsNs, psXLinkNs,
        "GetCoverage", OWS_METHOD_GET, script_url_encoded);

    format_list = msWCSGetFormatsList11( map, NULL );
    
    xmlAddChild(psMainNode, psNode);
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "service", "WCS"));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "version", (char *)params->version));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "Identifier", identifier_list ));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "InterpolationType", 
                    "NEAREST_NEIGHBOUR,BILINEAR" ));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "format", format_list ));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "store", "false" ));
    xmlAddChild(psNode, msOWSCommonOperationsMetadataDomainType(
                    psOwsNs, "Parameter", "GridBaseCRS", 
                    "urn:ogc:def:crs:epsg::4326" ));

    msFree( format_list );
    
/* -------------------------------------------------------------------- */
/*      Contents section.                                               */
/* -------------------------------------------------------------------- */
    psMainNode = xmlNewChild( psRootNode, NULL, "Contents", NULL );

    for(i=0; i<map->numlayers; i++)
    {
        layerObj *layer = map->layers[i];
        int       status;

        if(!msWCSIsLayerSupported(layer)) 
            continue;

        status = msWCSGetCapabilities11_CoverageSummary( 
            map, params, req, psDoc, psMainNode, layer );
        if(status != MS_SUCCESS) return MS_FAILURE;
    }

/* -------------------------------------------------------------------- */
/*      Write out the document.                                         */
/* -------------------------------------------------------------------- */

    if( msIO_needBinaryStdout() == MS_FAILURE )
        return MS_FAILURE;
     
    msIO_printf("Content-type: text/xml%c%c",10,10);
    
    context = msIO_getHandler(stdout);

    xmlDocDumpFormatMemoryEnc(psDoc, &buffer, &size, "ISO-8859-1", 1);
    msIO_contextWrite(context, buffer, size);
    xmlFree(buffer);

    /*free buffer and the document */
    /*xmlFree(buffer);*/
    xmlFreeDoc(psDoc);

    xmlCleanupParser();

    /* clean up */
    msWCSFreeParams(params);
    free(params);
    free( script_url_encoded );
    free( identifier_list );

    return(MS_SUCCESS);
}

/************************************************************************/
/*            msWCSDescribeCoverage_CoverageDescription11()             */
/************************************************************************/

static int 
msWCSDescribeCoverage_CoverageDescription11(
    layerObj *layer, wcsParamsObj *params, xmlNodePtr psRootNode,
    xmlNsPtr psOwsNs )

{
    int status;
    coverageMetadataObj cm;
    xmlNodePtr psCD, psDomain, psSD, psGridCRS;
    const char *value;

/* -------------------------------------------------------------------- */
/*      Verify layer is processable.                                    */
/* -------------------------------------------------------------------- */
    if( msCheckParentPointer(layer->map,"map") == MS_FAILURE )
	return MS_FAILURE;

    if(!msWCSIsLayerSupported(layer)) 
        return MS_SUCCESS;
    
/* -------------------------------------------------------------------- */
/*      Setup coverage metadata.                                        */
/* -------------------------------------------------------------------- */
    status = msWCSGetCoverageMetadata(layer, &cm);
    if(status != MS_SUCCESS) return status;

    /* fill in bands rangeset info, if required.  */
    msWCSSetDefaultBandsRangeSetInfo( params, &cm, layer );

/* -------------------------------------------------------------------- */
/*      Create CoverageDescription node.                                */
/* -------------------------------------------------------------------- */
    psCD = xmlNewChild( psRootNode, NULL, "CoverageDescription", NULL );
    
/* -------------------------------------------------------------------- */
/*      Title (from description)                                        */
/* -------------------------------------------------------------------- */
    value = msOWSLookupMetadata( &(layer->metadata), "COM", "description");
    if( value == NULL )
        value = layer->name;
    xmlNewChild( psCD, psOwsNs, "Title", value );

/* -------------------------------------------------------------------- */
/*      Identifier (layer name)                                         */
/* -------------------------------------------------------------------- */
    xmlNewChild( psCD, NULL, "Identifier", layer->name );

/* -------------------------------------------------------------------- */
/*      Keywords                                                        */
/* -------------------------------------------------------------------- */
    value = msOWSLookupMetadata(&(layer->metadata), "COM", "keywordlist");

    if (value)
        msLibXml2GenerateList( 
            xmlNewChild(psCD, psOwsNs, "Keywords", NULL),
            NULL, "Keyword", value, ',' );

/* -------------------------------------------------------------------- */
/*      Domain                                                          */
/* -------------------------------------------------------------------- */
    psDomain = xmlNewChild( psCD, NULL, "Domain", NULL );

/* -------------------------------------------------------------------- */
/*      SpatialDomain                                                   */
/* -------------------------------------------------------------------- */
    psSD = xmlNewChild( psDomain, NULL, "SpatialDomain", NULL );

/* -------------------------------------------------------------------- */
/*      imageCRS bounding box.                                          */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psSD,
        msOWSCommonBoundingBox( psOwsNs, "urn:ogc:def:crs:OGC::imageCRS",
                                2, 0, 0, cm.xsize-1, cm.ysize-1 ));

/* -------------------------------------------------------------------- */
/*      native CRS bounding box.                                        */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psSD,
        msOWSCommonBoundingBox( psOwsNs, cm.srs_urn, 2, 
                                cm.extent.minx, cm.extent.miny,
                                cm.extent.maxx, cm.extent.maxy ));

/* -------------------------------------------------------------------- */
/*      WGS84 bounding box.                                             */
/* -------------------------------------------------------------------- */
    xmlAddChild( 
        psSD,
        msOWSCommonWGS84BoundingBox( psOwsNs, 2,
                                     cm.llextent.minx, cm.llextent.miny,
                                     cm.llextent.maxx, cm.llextent.maxy ));

/* -------------------------------------------------------------------- */
/*      GridCRS                                                         */
/* -------------------------------------------------------------------- */
    {
        char format_buf[500];

        psGridCRS = xmlNewChild( psSD, NULL, "GridCRS", NULL );

        
        xmlNewChild( psGridCRS, NULL, "GridBaseCRS", cm.srs_urn );
        xmlNewChild( psGridCRS, NULL, "GridType", 
                     "urn:ogc:def:method:WCS:1.1:2dSimpleGrid" );

        sprintf( format_buf, "%.15g %.15g", 
                 cm.geotransform[0]+cm.geotransform[1]/2+cm.geotransform[2]/2, 
                 cm.geotransform[3]+cm.geotransform[4]/2+cm.geotransform[5]/2);
        xmlNewChild( psGridCRS, NULL, "GridOrigin", format_buf );

        sprintf( format_buf, "%.15g %.15g", 
                 cm.geotransform[1], cm.geotransform[5] );
        xmlNewChild( psGridCRS, NULL, "GridOffsets", format_buf );

        xmlNewChild( psGridCRS, NULL, "GridCS", 
                     "urn:ogc:def:cs:OGC:0.0:Grid2dSquareCS" );
    }



#ifdef notdef
  /* TemporalDomain */

  /* TODO: figure out when a temporal domain is valid, for example only tiled rasters support time as a domain, plus we need a timeitem */
  if(msOWSLookupMetadata(&(layer->metadata), "COM", "timeposition") || msOWSLookupMetadata(&(layer->metadata), "COM", "timeperiod")) {
    msIO_printf("      <temporalDomain>\n");

    /* TimePosition (should support a value AUTO, then we could mine positions from the timeitem) */
    msOWSPrintEncodeMetadataList(stdout, &(layer->metadata), "COM", "timeposition", NULL, NULL, "        <gml:timePosition>%s</gml:timePosition>\n", NULL);    

    /* TODO:  add TimePeriod (only one per layer)  */

    msIO_printf("      </temporalDomain>\n");
  }
  
  msIO_printf("    </domainSet>\n");
#endif

/* -------------------------------------------------------------------- */
/*      Range                                                           */
/* -------------------------------------------------------------------- */
    {
        xmlNodePtr psField, psInterpMethods, psAxis;
        const char *value;

        psField = 
            xmlNewChild(
                xmlNewChild( psCD, NULL, "Range", NULL ),
                NULL, "Field", NULL );
        
        value = msOWSGetEncodeMetadata( &(layer->metadata), "COM", 
                                        "rangeset_label", NULL );
        if( value )
            xmlNewChild( psField, psOwsNs, "Title", value );

        /* ows:Abstract? TODO */

        value = msOWSGetEncodeMetadata( &(layer->metadata), "COM", 
                                        "rangeset_name", "bands" );
        xmlNewChild( psField, NULL, "Identifier", value );
        
        /* <NullValue> TODO */
        
        psInterpMethods = 
            xmlNewChild( psField, NULL, "InterpolationMethods", NULL );

        xmlNewChild( psInterpMethods, NULL, 
                     "DefaultMethod", "nearest neighbour" );
        xmlNewChild( psInterpMethods, NULL, "OtherMethod", "bilinear" );

/* -------------------------------------------------------------------- */
/*      Do axes properly later...                                       */
/* -------------------------------------------------------------------- */
        psAxis = xmlNewChild( psField, NULL, "Axis", NULL );
        xmlNewProp( psAxis, "identifier", "Band" );

        msLibXml2GenerateList( 
            xmlNewChild(psAxis, NULL, "AvailableKeys", NULL),
            NULL, "Key", "1", ',' );
        
    }        
        
#ifdef notdef
  /* compound range sets */
  if((value = msOWSLookupMetadata(&(layer->metadata), "COM", "rangeset_axes")) != NULL) {
     tokens = msStringSplit(value, ',', &numtokens);
     if(tokens && numtokens > 0) {
       for(i=0; i<numtokens; i++)
         msWCSDescribeCoverage_AxisDescription(layer, tokens[i]);
       msFreeCharArray(tokens, numtokens);
     }
  }
#endif  

/* -------------------------------------------------------------------- */
/*      SupportedCRS                                                    */
/* -------------------------------------------------------------------- */
    {
        char *owned_value;
        
        if( (owned_value = 
             msOWSGetProjURN( &(layer->projection), &(layer->metadata), 
                              "COM", MS_FALSE)) != NULL ) {
            /* ok */
        } else if((owned_value = 
                   msOWSGetProjURN( &(layer->map->projection), 
                                    &(layer->map->web.metadata), 
                                    "COM", MS_FALSE)) != NULL ) {
            /* ok */
        } else 
            msDebug( "mapwcs.c: missing required information, no SRSs defined.");
        
        if( owned_value != NULL && strlen(owned_value) > 0 ) 
            msLibXml2GenerateList( psCD, NULL, "SupportedCRS", 
                                    owned_value, ' ' );

        msFree( owned_value );
    }

/* -------------------------------------------------------------------- */
/*      SupportedFormats                                                */
/* -------------------------------------------------------------------- */
    {
        char *format_list;
        
        format_list = msWCSGetFormatsList11( layer->map, layer );
        
        if (strlen(format_list) > 0 )
            msLibXml2GenerateList( psCD, NULL, "SupportedFormat", 
                                    format_list, ',' );
        
        msFree( format_list );
    }
    
    return MS_SUCCESS;
}

/************************************************************************/
/*                      msWCSDescribeCoverage11()                       */
/************************************************************************/

int msWCSDescribeCoverage11(mapObj *map, wcsParamsObj *params)
{
    xmlDocPtr psDoc = NULL;       /* document pointer */
    xmlNodePtr psRootNode;
    xmlNsPtr psOwsNs, psXLinkNs;
    int i,j;

/* -------------------------------------------------------------------- */
/*      We will actually get the coverages list as a single item in     */
/*      a string list with that item having the comma delimited         */
/*      coverage names.  Split it up now, and assign back in place      */
/*      of the old coverages list.                                      */
/* -------------------------------------------------------------------- */
    if( CSLCount(params->coverages) == 1 )
    {
        char **old_coverages = params->coverages;
        params->coverages = CSLTokenizeStringComplex( old_coverages[0], ",",
                                                      FALSE, FALSE );
        CSLDestroy( old_coverages );
    }

/* -------------------------------------------------------------------- */
/*      Validate that the requested coverages exist as named layers.    */
/* -------------------------------------------------------------------- */
    if(params->coverages) { /* use the list */
        for( j = 0; params->coverages[j]; j++ ) {
            i = msGetLayerIndex(map, params->coverages[j]);
            if(i == -1) {
                msSetError( MS_WCSERR,
                            "COVERAGE %s cannot be opened / does not exist",
                            "msWCSDescribeCoverage()", params->coverages[j]);
                return msWCSException(map, params->coverages[j], "CoverageNotDefined", "coverage");
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Create document.                                                */
/* -------------------------------------------------------------------- */
    psDoc = xmlNewDoc("1.0");

    psRootNode = xmlNewNode(NULL, "CoverageDescriptions");

    xmlDocSetRootElement(psDoc, psRootNode);

/* -------------------------------------------------------------------- */
/*      Name spaces                                                     */
/* -------------------------------------------------------------------- */
    xmlSetNs(psRootNode, xmlNewNs(psRootNode, "http://www.opengis.net/wcs/1.1", NULL));
    psOwsNs = xmlNewNs(psRootNode, MS_OWSCOMMON_OWS_NAMESPACE_URI, MS_OWSCOMMON_OWS_NAMESPACE_PREFIX);
    psXLinkNs = xmlNewNs(psRootNode, MS_OWSCOMMON_W3C_XLINK_NAMESPACE_URI, MS_OWSCOMMON_W3C_XLINK_NAMESPACE_PREFIX);
    xmlNewNs(psRootNode, MS_OWSCOMMON_W3C_XSI_NAMESPACE_URI, MS_OWSCOMMON_W3C_XSI_NAMESPACE_PREFIX);
    xmlNewNs(psRootNode, MS_OWSCOMMON_OGC_NAMESPACE_URI, MS_OWSCOMMON_OGC_NAMESPACE_PREFIX );

    xmlNewProp(psRootNode, "version", params->version );

/* -------------------------------------------------------------------- */
/*      Generate a CoverageDescription for each requested coverage.     */
/* -------------------------------------------------------------------- */

    if(params->coverages) { /* use the list */
        for( j = 0; params->coverages[j]; j++ ) {
            i = msGetLayerIndex(map, params->coverages[j]);
            msWCSDescribeCoverage_CoverageDescription11((GET_LAYER(map, i)), 
                                                        params, psRootNode,
                                                        psOwsNs );
        }
    } else { /* return all layers */
        for(i=0; i<map->numlayers; i++)
            msWCSDescribeCoverage_CoverageDescription11((GET_LAYER(map, i)), 
                                                        params, psRootNode,
                                                        psOwsNs );
    }
  
/* -------------------------------------------------------------------- */
/*      Write out the document.                                         */
/* -------------------------------------------------------------------- */
    {
        xmlChar *buffer = NULL;
        int size = 0;
        msIOContext *context = NULL;

        if( msIO_needBinaryStdout() == MS_FAILURE )
            return MS_FAILURE;
     
        msIO_printf("Content-type: text/xml%c%c",10,10);
    
        context = msIO_getHandler(stdout);

        xmlDocDumpFormatMemoryEnc(psDoc, &buffer, &size, "ISO-8859-1", 1);
        msIO_contextWrite(context, buffer, size);
        xmlFree(buffer);
    }
        
/* -------------------------------------------------------------------- */
/*      Cleanup                                                         */
/* -------------------------------------------------------------------- */
    xmlFreeDoc(psDoc);
    xmlCleanupParser();
    msWCSFreeParams(params);

    free(params);

    return MS_SUCCESS;
}

#endif /* defined(USE_WCS_SVR) && defined(USE_LIBXML2) */

/************************************************************************/
/*                       msWCSReturnCoverage11()                        */
/*                                                                      */
/*      Return a render image as a coverage to the caller with WCS      */
/*      1.1 "mime" wrapping.                                            */
/************************************************************************/

#if defined(USE_WCS_SVR)
int  msWCSReturnCoverage11( wcsParamsObj *params, mapObj *map, 
                            imageObj *image )
{
    int status;

    msIO_fprintf( 
        stdout, 
        "Content-Type: multipart/mixed; boundary=wcs%c%c"
        "--wcs\n"
        "Content-Type: text/xml\n"
        "Content-ID: wcs.xml%c%c"
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Coverages\n"
        "     xmlns=\"http://www.opengis.net/wcs/1.1\"\n"
        "     xmlns:ows=\"http://www.opengis.net/ows\"\n"
        "     xmlns:xlink=\"http://www.w3.org/1999/xlink\"\n"
        "     xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n"
        "     xsi:schemaLocation=\"http://www.opengis.net/ows/1.1 ../owsCoverages.xsd\">\n"
        "  <Coverage>\n"
        "    <Reference xlink:href=\"cid:coverage/wcs.%s\"/>\n"
        "  </Coverage>\n"
        "</Coverages>\n"
        "--wcs\n"
        "Content-Type: %s\n"
        "Content-Description: coverage data\n"
        "Content-Transfer-Encoding: binary\n"
        "Content-ID: coverage/wcs.%s\n"
        "Content-Disposition: INLINE%c%c",
        10, 10, 
        10, 10,
        MS_IMAGE_EXTENSION(map->outputformat),
        MS_IMAGE_MIME_TYPE(map->outputformat),
        MS_IMAGE_EXTENSION(map->outputformat),
        10, 10 );

      status = msSaveImage(map, image, NULL);
      if( status != MS_SUCCESS )
      {
          return msWCSException(map, params->version, NULL, NULL);
      }

      msIO_fprintf( stdout, "--wcs--%c%c", 10, 10 );

      return MS_SUCCESS;
}
#endif

/************************************************************************/
/* ==================================================================== */
/*	If we don't have libxml2 but WCS SVR was selected, then         */
/*      report WCS 1.1 requests as unsupported.                         */
/* ==================================================================== */
/************************************************************************/

#if defined(USE_WCS_SVR) && !defined(USE_LIBXML2)

#include "mapwcs.h"

/* ==================================================================== */

int msWCSDescribeCoverage11(mapObj *map, wcsParamsObj *params)

{
    msSetError( MS_WCSERR,
                "WCS 1.1 request made, but mapserver requires libxml2 for WCS 1.1 services and this is not configured.",
                "msWCSDescribeCoverage11()", "NoApplicableCode" );

    return msWCSException(map, params->version, 
                          "NoApplicableCode", "NoApplicableCode");
}

/* ==================================================================== */

int msWCSGetCapabilities11(mapObj *map, wcsParamsObj *params, 
                                  cgiRequestObj *req)

{
    msSetError( MS_WCSERR,
                "WCS 1.1 request made, but mapserver requires libxml2 for WCS 1.1 services and this is not configured.",
                "msWCSGetCapabilities11()", "NoApplicableCode" );

    return msWCSException(map, params->version, 
                          "NoApplicableCode", "NoApplicableCode");
}

#endif /* defined(USE_WCS_SVR) && !defined(USE_LIBXML2) */
/* GStreamer
 * Copyright (C) 2003 Benjamin Otte <in7y118@public.uni-hamburg.de>
 * Copyright (C) 2005 Thomas Vander Stichele <thomas at apestaart dot org>
 * Copyright (C) 2005 Wim Taymans <wim at fluendo dot com>
 *
 * gstaudioconvert.c: Convert audio to different audio formats automatically
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-audioconvert
 *
 * <refsect2>
 * <para>
 * Audioconvert converts raw audio buffers between various possible formats.
 * It supports integer to float conversion, width/depth conversion,
 * signedness and endianness conversion.
 * </para>
 * <para>
 * Some format conversion are not carried out in an optimal way right now.
 * E.g. converting from double to float would cause a loss of precision.
 * </para>
 * <title>Example launch line</title>
 * <para>
 * <programlisting>
 * gst-launch -v -m audiotestsrc ! audioconvert ! audio/x-raw-int,channels=2,width=8,depth=8 ! level ! fakesink silent=TRUE
 * </programlisting>
 * This pipeline converts audio to 8-bit.  The level element shows that
 * the output levels still match the one for a sine wave.
 * </para>
 * <para>
 * <programlisting>
 * gst-launch -v -m audiotestsrc ! audioconvert ! vorbisenc ! fakesink silent=TRUE
 * </programlisting>
 * The vorbis encoder takes float audio data instead of the integer data
 * generated by audiotestsrc.
 * </para>
 * </refsect2>
 *
 * Last reviewed on 2006-03-02 (0.10.4)
 */

/*
 * design decisions:
 * - audioconvert converts buffers in a set of supported caps. If it supports
 *   a caps, it supports conversion from these caps to any other caps it
 *   supports. (example: if it does A=>B and A=>C, it also does B=>C)
 * - audioconvert does not save state between buffers. Every incoming buffer is
 *   converted and the converted buffer is pushed out.
 * conclusion:
 * audioconvert is not supposed to be a one-element-does-anything solution for
 * audio conversions.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstaudioconvert.h"
#include "gstchannelmix.h"
#include "plugin.h"

GST_DEBUG_CATEGORY (audio_convert_debug);

/*** DEFINITIONS **************************************************************/

static const GstElementDetails audio_convert_details =
GST_ELEMENT_DETAILS ("Audio converter",
    "Filter/Converter/Audio",
    "Convert audio to different formats",
    "Benjamin Otte <in7y118@public.uni-hamburg.de>");

/* type functions */
static void gst_audio_convert_dispose (GObject * obj);

/* gstreamer functions */
static gboolean gst_audio_convert_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, guint * size);
static GstCaps *gst_audio_convert_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps);
static void gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_audio_convert_set_caps (GstBaseTransform * base,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_audio_convert_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static GstFlowReturn gst_audio_convert_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);

/* AudioConvert signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_AGGRESSIVE
};

#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (audio_convert_debug, "audioconvert", 0, "audio conversion element");

GST_BOILERPLATE_FULL (GstAudioConvert, gst_audio_convert, GstBaseTransform,
    GST_TYPE_BASE_TRANSFORM, DEBUG_INIT);

/*** GSTREAMER PROTOTYPES *****************************************************/

#define STATIC_CAPS \
GST_STATIC_CAPS ( \
  "audio/x-raw-float, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], " \
    "endianness = (int) BYTE_ORDER, " \
    "width = (int) 64;" \
  "audio/x-raw-float, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], " \
    "endianness = (int) BYTE_ORDER, " \
    "width = (int) 32;" \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 32, " \
    "depth = (int) [ 1, 32 ], " \
    "signed = (boolean) { true, false }; " \
  "audio/x-raw-int, "   \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], "       \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, "        \
    "width = (int) 24, "        \
    "depth = (int) [ 1, 24 ], " "signed = (boolean) { true, false }; "  \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 16, " \
    "depth = (int) [ 1, 16 ], " \
    "signed = (boolean) { true, false }; " \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, 8 ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 8, " \
    "depth = (int) [ 1, 8 ], " \
    "signed = (boolean) { true, false } " \
)

static GstAudioChannelPosition *supported_positions;

static GstStaticPadTemplate gst_audio_convert_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    STATIC_CAPS);

static GstStaticPadTemplate gst_audio_convert_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    STATIC_CAPS);

/*** TYPE FUNCTIONS ***********************************************************/

static void
gst_audio_convert_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_sink_template));
  gst_element_class_set_details (element_class, &audio_convert_details);
}

static void
gst_audio_convert_class_init (GstAudioConvertClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gint i;

  gobject_class->dispose = gst_audio_convert_dispose;

  supported_positions = g_new0 (GstAudioChannelPosition,
      GST_AUDIO_CHANNEL_POSITION_NUM);
  for (i = 0; i < GST_AUDIO_CHANNEL_POSITION_NUM; i++)
    supported_positions[i] = i;

  GST_BASE_TRANSFORM_CLASS (klass)->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_audio_convert_get_unit_size);
  GST_BASE_TRANSFORM_CLASS (klass)->transform_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_fixate_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->set_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_set_caps);
  GST_BASE_TRANSFORM_CLASS (klass)->transform_ip =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_ip);
  GST_BASE_TRANSFORM_CLASS (klass)->transform =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform);

  GST_BASE_TRANSFORM_CLASS (klass)->passthrough_on_same_caps = TRUE;
}

static void
gst_audio_convert_init (GstAudioConvert * this, GstAudioConvertClass * g_class)
{
}

static void
gst_audio_convert_dispose (GObject * obj)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (obj);

  audio_convert_clean_context (&this->ctx);

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

/*** GSTREAMER FUNCTIONS ******************************************************/

/* convert the given GstCaps to our format */
static gboolean
gst_audio_convert_parse_caps (const GstCaps * caps, AudioConvertFmt * fmt)
{
  GstStructure *structure = gst_caps_get_structure (caps, 0);

  GST_DEBUG ("parse caps %p and %" GST_PTR_FORMAT, caps, caps);

  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);
  g_return_val_if_fail (fmt != NULL, FALSE);

  /* cleanup old */
  audio_convert_clean_fmt (fmt);

  fmt->endianness = G_BYTE_ORDER;
  fmt->is_int =
      (strcmp (gst_structure_get_name (structure), "audio/x-raw-int") == 0);

  /* parse common fields */
  if (!gst_structure_get_int (structure, "channels", &fmt->channels))
    goto no_values;
  if (!(fmt->pos = gst_audio_get_channel_positions (structure)))
    goto no_values;
  if (!gst_structure_get_int (structure, "width", &fmt->width))
    goto no_values;
  if (!gst_structure_get_int (structure, "rate", &fmt->rate))
    goto no_values;

  if (fmt->is_int) {
    /* int specific fields */
    if (!gst_structure_get_boolean (structure, "signed", &fmt->sign))
      goto no_values;
    if (!gst_structure_get_int (structure, "depth", &fmt->depth))
      goto no_values;

    /* width != 8 can have an endianness field */
    if (fmt->width != 8) {
      if (!gst_structure_get_int (structure, "endianness", &fmt->endianness))
        goto no_values;
    }
    /* depth cannot be bigger than the width */
    if (fmt->depth > fmt->width)
      goto not_allowed;
  }

  fmt->unit_size = (fmt->width * fmt->channels) / 8;

  return TRUE;

  /* ERRORS */
no_values:
  {
    GST_DEBUG ("could not get some values from structure");
    audio_convert_clean_fmt (fmt);
    return FALSE;
  }
not_allowed:
  {
    GST_DEBUG ("width > depth, not allowed - make us advertise correct fmt");
    audio_convert_clean_fmt (fmt);
    return FALSE;
  }
}

/* BaseTransform vmethods */
static gboolean
gst_audio_convert_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    guint * size)
{
  AudioConvertFmt fmt = { 0 };

  g_assert (size);

  if (!gst_audio_convert_parse_caps (caps, &fmt))
    goto parse_error;

  *size = fmt.unit_size;

  audio_convert_clean_fmt (&fmt);

  return TRUE;

parse_error:
  {
    return FALSE;
  }
}

/* Set widths (a list); multiples of 8 between min and max */
static void
set_structure_widths (GstStructure * s, int min, int max)
{
  GValue list = { 0 };
  GValue val = { 0 };
  int width;

  if (min == max) {
    gst_structure_set (s, "width", G_TYPE_INT, min, NULL);
    return;
  }

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_INT);
  for (width = min; width <= max; width += 8) {
    g_value_set_int (&val, width);
    gst_value_list_append_value (&list, &val);
  }
  gst_structure_set_value (s, "width", &list);
  g_value_unset (&val);
  g_value_unset (&list);
}

/* Set widths of 32 bits and 64 bits (as list) */
static void
set_structure_widths_32_and_64 (GstStructure * s)
{
  GValue list = { 0 };
  GValue val = { 0 };

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_INT);
  g_value_set_int (&val, 32);
  gst_value_list_append_value (&list, &val);
  g_value_set_int (&val, 64);
  gst_value_list_append_value (&list, &val);
  gst_structure_set_value (s, "width", &list);
  g_value_unset (&val);
  g_value_unset (&list);
}

/* Modify the structure so that things that must always have a single
 * value (for float), or can always be losslessly converted (for int), have
 * appropriate values.
 */
static GstStructure *
make_lossless_changes (GstStructure * s, gboolean isfloat)
{
  if (isfloat) {
    /* float doesn't have a depth or signedness field and only supports a
     * widths of 32/64 and native endianness */
    gst_structure_remove_field (s, "depth");
    gst_structure_remove_field (s, "signed");
    set_structure_widths_32_and_64 (s);
    gst_structure_set (s, "endianness", G_TYPE_INT, G_BYTE_ORDER, NULL);
  } else {
    /* int supports either endian, and signed or unsigned. GValues are a pain */
    GValue list = { 0 };
    GValue val = { 0 };
    int i;
    const gint endian[] = { G_LITTLE_ENDIAN, G_BIG_ENDIAN };
    const gboolean booleans[] = { TRUE, FALSE };

    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&val, G_TYPE_INT);
    for (i = 0; i < 2; i++) {
      g_value_set_int (&val, endian[i]);
      gst_value_list_append_value (&list, &val);
    }
    gst_structure_set_value (s, "endianness", &list);
    g_value_unset (&val);
    g_value_unset (&list);

    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&val, G_TYPE_BOOLEAN);
    for (i = 0; i < 2; i++) {
      g_value_set_boolean (&val, booleans[i]);
      gst_value_list_append_value (&list, &val);
    }
    gst_structure_set_value (s, "signed", &list);
    g_value_unset (&val);
    g_value_unset (&list);
  }

  return s;
}

/* Little utility function to create a related structure for float/int */
static void
append_with_other_format (GstCaps * caps, GstStructure * s, gboolean isfloat)
{
  GstStructure *s2;

  if (isfloat) {
    s2 = gst_structure_copy (s);
    gst_structure_set_name (s2, "audio/x-raw-int");
    s = make_lossless_changes (s2, FALSE);
    gst_caps_append_structure (caps, s2);
  } else {
    s2 = gst_structure_copy (s);
    gst_structure_set_name (s2, "audio/x-raw-float");
    s = make_lossless_changes (s2, TRUE);
    gst_caps_append_structure (caps, s2);
  }
}

/* Audioconvert can perform all conversions on audio except for resampling. 
 * However, there are some conversions we _prefer_ not to do. For example, it's
 * better to convert format (float<->int, endianness, etc) than the number of
 * channels, as the latter conversion is not lossless.
 *
 * So, we return, in order (assuming input caps have only one structure; 
 * which is enforced by basetransform):
 *  - input caps with a different format (lossless conversions).
 *  - input caps with a different format (slightly lossy conversions).
 *  - input caps with a different number of channels (very lossy!)
 */
static GstCaps *
gst_audio_convert_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps)
{
  GstCaps *ret;
  GstStructure *s, *structure;
  gboolean isfloat;
  gint width, depth, channels;
  const gchar *fields_used[] = {
    "width", "depth", "rate", "channels", "endianness", "signed"
  };
  const gchar *structure_name;
  int i;

  g_return_val_if_fail (GST_CAPS_IS_SIMPLE (caps), NULL);

  structure = gst_caps_get_structure (caps, 0);
  structure_name = gst_structure_get_name (structure);

  isfloat = strcmp (structure_name, "audio/x-raw-float") == 0;

  /* We operate on a version of the original structure with any additional
   * fields absent */
  s = gst_structure_empty_new (structure_name);
  for (i = 0; i < sizeof (fields_used) / sizeof (*fields_used); i++) {
    if (gst_structure_has_field (structure, fields_used[i]))
      gst_structure_set_value (s, fields_used[i],
          gst_structure_get_value (structure, fields_used[i]));
  }

  if (!isfloat) {
    /* Commonly, depth is left out: set it equal to width if we have a fixed
     * width, if so */
    if (!gst_structure_has_field (s, "depth") &&
        gst_structure_get_int (s, "width", &width))
      gst_structure_set (s, "depth", G_TYPE_INT, width, NULL);
  }

  ret = gst_caps_new_empty ();

  /* All lossless conversions */
  s = make_lossless_changes (s, isfloat);
  gst_caps_append_structure (ret, s);

  /* Same, plus a float<->int conversion */
  append_with_other_format (ret, s, isfloat);
  GST_DEBUG_OBJECT (base, "  step1: (%d) %" GST_PTR_FORMAT,
      gst_caps_get_size (ret), ret);

  /* We don't mind increasing width/depth/channels, but reducing them is 
   * Very Bad. Only available if width, depth, channels are already fixed. */
  s = gst_structure_copy (s);
  if (!isfloat) {
    if (gst_structure_get_int (structure, "width", &width))
      set_structure_widths (s, width, 32);
    if (gst_structure_get_int (structure, "depth", &depth)) {
      if (depth == 32)
        gst_structure_set (s, "depth", G_TYPE_INT, 32, NULL);
      else
        gst_structure_set (s, "depth", GST_TYPE_INT_RANGE, depth, 32, NULL);
    }
  }

  if (gst_structure_get_int (structure, "channels", &channels)) {
    if (channels == 8)
      gst_structure_set (s, "channels", G_TYPE_INT, 8, NULL);
    else
      gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, channels, 8, NULL);
  }
  gst_caps_append_structure (ret, s);

  /* Same, plus a float<->int conversion */
  append_with_other_format (ret, s, isfloat);

  /* We'll reduce depth if we must... only for integer, since we can't do this
   * for float. We reduce as low as 16 bits; reducing to less than this is
   * even worse than dropping channels. We only do this if we haven't already
   * done the equivalent above. */
  if (!gst_structure_get_int (structure, "width", &width) || width > 16) {
    if (isfloat) {
      /* These are invalid widths/depths for float, but we don't actually use
       * them - we just pass it to append_with_other_format, which makes them
       * valid
       */
      GstStructure *s2 = gst_structure_copy (s);

      set_structure_widths (s2, 16, 32);
      gst_structure_set (s2, "depth", GST_TYPE_INT_RANGE, 16, 32, NULL);
      append_with_other_format (ret, s2, TRUE);
      gst_structure_free (s2);
    } else {
      s = gst_structure_copy (s);
      set_structure_widths (s, 16, 32);
      gst_structure_set (s, "depth", GST_TYPE_INT_RANGE, 16, 32, NULL);
      gst_caps_append_structure (ret, s);
    }
  }

  /* Channel conversions to fewer channels is only done if needed - generally
   * it's very bad to drop channels entirely.
   */
  s = gst_structure_copy (s);
  gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, 1, 8, NULL);
  gst_caps_append_structure (ret, s);

  /* Same, plus a float<->int conversion */
  append_with_other_format (ret, s, isfloat);

  /* And, finally, for integer only, we allow conversion to any width/depth we
   * support: this should be equivalent to our (non-float) template caps. (the
   * floating point case should be being handled just above) */
  s = gst_structure_copy (s);
  set_structure_widths (s, 8, 32);
  gst_structure_set (s, "depth", GST_TYPE_INT_RANGE, 1, 32, NULL);

  if (isfloat) {
    append_with_other_format (ret, s, TRUE);
    gst_structure_free (s);
  } else
    gst_caps_append_structure (ret, s);

  return ret;
}

/* try to keep as many of the structure members the same by fixating the
 * possible ranges; this way we convert the least amount of things as possible
 */
static void
gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  gint rate, endianness, depth, width, channels;
  gboolean signedness;

  g_return_if_fail (gst_caps_is_fixed (caps));

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  if (gst_structure_get_int (ins, "channels", &channels)) {
    if (gst_structure_has_field (outs, "channels")) {
      gst_structure_fixate_field_nearest_int (outs, "channels", channels);
    }
  }
  if (gst_structure_get_int (ins, "rate", &rate)) {
    if (gst_structure_has_field (outs, "rate")) {
      gst_structure_fixate_field_nearest_int (outs, "rate", rate);
    }
  }
  if (gst_structure_get_int (ins, "endianness", &endianness)) {
    if (gst_structure_has_field (outs, "endianness")) {
      gst_structure_fixate_field_nearest_int (outs, "endianness", endianness);
    }
  }
  if (gst_structure_get_int (ins, "width", &width)) {
    if (gst_structure_has_field (outs, "width")) {
      gst_structure_fixate_field_nearest_int (outs, "width", width);
    }
  } else {
    /* this is not allowed */
  }

  if (gst_structure_get_int (ins, "depth", &depth)) {
    if (gst_structure_has_field (outs, "depth")) {
      gst_structure_fixate_field_nearest_int (outs, "depth", depth);
    }
  } else {
    /* set depth as width */
    if (gst_structure_has_field (outs, "depth")) {
      gst_structure_fixate_field_nearest_int (outs, "depth", width);
    }
  }

  if (gst_structure_get_boolean (ins, "signed", &signedness)) {
    if (gst_structure_has_field (outs, "signed")) {
      gst_structure_fixate_field_boolean (outs, "signed", signedness);
    }
  }

  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);
}

static gboolean
gst_audio_convert_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  AudioConvertFmt in_ac_caps = { 0 };
  AudioConvertFmt out_ac_caps = { 0 };
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);

  GST_DEBUG_OBJECT (base, "incaps %" GST_PTR_FORMAT ", outcaps %"
      GST_PTR_FORMAT, incaps, outcaps);

  if (!gst_audio_convert_parse_caps (incaps, &in_ac_caps))
    return FALSE;
  if (!gst_audio_convert_parse_caps (outcaps, &out_ac_caps))
    return FALSE;

  if (!audio_convert_prepare_context (&this->ctx, &in_ac_caps, &out_ac_caps))
    goto no_converter;

  return TRUE;

no_converter:
  {
    return FALSE;
  }
}

static GstFlowReturn
gst_audio_convert_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  /* nothing to do here */
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_audio_convert_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);
  gboolean res;
  gint insize, outsize;
  gint samples;
  gpointer src, dst;

  /* get amount of samples to convert. */
  samples = GST_BUFFER_SIZE (inbuf) / this->ctx.in.unit_size;

  /* get in/output sizes, to see if the buffers we got are of correct
   * sizes */
  if (!(res = audio_convert_get_sizes (&this->ctx, samples, &insize, &outsize)))
    goto error;

  if (insize == 0 || outsize == 0)
    return GST_FLOW_OK;

  /* check in and outsize */
  if (GST_BUFFER_SIZE (inbuf) < insize)
    goto wrong_size;
  if (GST_BUFFER_SIZE (outbuf) < outsize)
    goto wrong_size;

  /* get src and dst data */
  src = GST_BUFFER_DATA (inbuf);
  dst = GST_BUFFER_DATA (outbuf);

  /* and convert the samples */
  if (!(res = audio_convert_convert (&this->ctx, src, dst,
              samples, gst_buffer_is_writable (inbuf))))
    goto convert_error;

  GST_BUFFER_SIZE (outbuf) = outsize;

  return GST_FLOW_OK;

  /* ERRORS */
error:
  {
    GST_ELEMENT_ERROR (this, STREAM, NOT_IMPLEMENTED,
        ("cannot get input/output sizes for %d samples", samples),
        ("cannot get input/output sizes for %d samples", samples));
    return GST_FLOW_ERROR;
  }
wrong_size:
  {
    GST_ELEMENT_ERROR (this, STREAM, NOT_IMPLEMENTED,
        ("input/output buffers are of wrong size in: %d < %d or out: %d < %d",
            GST_BUFFER_SIZE (inbuf), insize, GST_BUFFER_SIZE (outbuf), outsize),
        ("input/output buffers are of wrong size in: %d < %d or out: %d < %d",
            GST_BUFFER_SIZE (inbuf), insize, GST_BUFFER_SIZE (outbuf),
            outsize));
    return GST_FLOW_ERROR;
  }
convert_error:
  {
    GST_ELEMENT_ERROR (this, STREAM, NOT_IMPLEMENTED,
        ("error while converting"), ("error while converting"));
    return GST_FLOW_ERROR;
  }
}

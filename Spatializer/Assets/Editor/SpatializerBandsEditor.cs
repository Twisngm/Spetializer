using UnityEditor;
using UnityEngine;

[CustomEditor(typeof(SpatializerBands))]
[CanEditMultipleObjects]
internal sealed class SpatializerBandsEditor : Editor
{
    private const float MinHz = 20f;
    private const float MaxHz = 20000f;
    private const float MinDb = -36f;
    private static readonly float MaxDb = 20f * Mathf.Log10(4f);
    private static readonly string[] BandNames = { "Low", "Low Mid", "High Mid", "High" };
    private static readonly string[] BandPaths = { "low", "lowMid", "highMid", "high" };
    private static readonly string[] CrossoverNames = { "Low / Low Mid", "Low Mid / High Mid", "High Mid / High" };
    private static readonly string[] CrossoverPaths = { "lowCrossover", "midCrossover", "highCrossover" };
    private static readonly float[] CrossoverDefaults =
    {
        SpatializerBands.DefaultLowCrossover, SpatializerBands.DefaultMidCrossover, SpatializerBands.DefaultHighCrossover
    };
    private static readonly Color[] BandColors =
    {
        new Color32(245, 112, 118, 255), new Color32(245, 180, 79, 255),
        new Color32(88, 217, 174, 255), new Color32(104, 179, 251, 255)
    };
    private static readonly Color CrossoverColor = new Color32(176, 191, 208, 255);
    private static readonly Color GraphText = new Color32(197, 208, 221, 255);
    private static readonly float[] FrequencyTicks = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    private static readonly string[] FrequencyLabels = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
    private static readonly float[] GainTicks = { -36, -24, -12, 0, 12 };

    private readonly SerializedProperty[] crossovers = new SerializedProperty[3];
    private readonly SerializedProperty[] gains = new SerializedProperty[4];
    private readonly SerializedProperty[] drives = new SerializedProperty[4];
    private readonly SerializedProperty[] wets = new SerializedProperty[4];
    private readonly Vector3[] curve = new Vector3[161];
    private SerializedProperty enabledBands;
    private SerializedProperty motionSmoothing;
    private GUIStyle axisLabel;
    private Vector2 dragOrigin;
    private float dragValue;
    private int undoGroup;

    private void OnEnable()
    {
        enabledBands = serializedObject.FindProperty("enableFourBandProcessing");
        motionSmoothing = serializedObject.FindProperty("motionSmoothingMs");
        for (int i = 0; i < crossovers.Length; i++)
            crossovers[i] = serializedObject.FindProperty(CrossoverPaths[i]);
        for (int i = 0; i < gains.Length; i++)
        {
            SerializedProperty band = serializedObject.FindProperty(BandPaths[i]);
            gains[i] = band.FindPropertyRelative("gain");
            drives[i] = band.FindPropertyRelative("saturationDrive");
            wets[i] = band.FindPropertyRelative("saturationWet");
        }
    }

    public override void OnInspectorGUI()
    {
        if (targets.Length > 1)
        {
            DrawDefaultInspector();
            return;
        }

        serializedObject.Update();
        EditorGUILayout.PropertyField(enabledBands, new GUIContent("Enable Four Band Processing"));
        EditorGUILayout.LabelField(enabledBands.boolValue ? "4-band response" : "4-band response preview (bypassed)",
            EditorStyles.boldLabel);
        DrawGraph(GUILayoutUtility.GetRect(160f, 230f, GUILayout.ExpandWidth(true)));
        EditorGUILayout.LabelField("Drag dots for gain and diamonds for crossover.", EditorStyles.miniLabel);
        EditorGUILayout.Space(4f);

        int columns = EditorGUIUtility.currentViewWidth >= 420f ? 4 : 2;
        for (int first = 0; first < gains.Length; first += columns)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                for (int i = first; i < first + columns; i++)
                    DrawBand(i);
            }
        }

        EditorGUILayout.Space(4f);
        EditorGUILayout.LabelField("Crossovers", EditorStyles.boldLabel);
        using (new EditorGUILayout.HorizontalScope())
        {
            for (int i = 0; i < crossovers.Length; i++)
            {
                GetCrossoverLimits(i, out float min, out float max);
                using (new EditorGUILayout.VerticalScope(EditorStyles.helpBox, GUILayout.ExpandWidth(true)))
                {
                    DrawKnob(crossovers[i], new GUIContent("F" + (i + 1) + " (Hz)", CrossoverNames[i]),
                        min, max, CrossoverDefaults[i], CrossoverColor, 25f, true);
                }
            }
        }
        EditorGUILayout.PropertyField(motionSmoothing);
        serializedObject.ApplyModifiedProperties();
    }

    private void DrawBand(int band)
    {
        using (new EditorGUILayout.VerticalScope(EditorStyles.helpBox, GUILayout.ExpandWidth(true)))
        {
            Rect header = GUILayoutUtility.GetRect(0f, 18f, GUILayout.ExpandWidth(true));
            EditorGUI.DrawRect(new Rect(header.x + 2f, header.y + 4f, 3f, 10f), BandColors[band]);
            GUI.Label(new Rect(header.x + 9f, header.y, header.width - 9f, header.height),
                BandNames[band], EditorStyles.miniBoldLabel);
            DrawKnob(gains[band], new GUIContent("Gain (×)"), 0f, 4f,
                SpatializerBands.DefaultGain, BandColors[band], 34f);
            using (new EditorGUILayout.HorizontalScope())
            {
                DrawKnob(drives[band], new GUIContent("Drive", "Saturation Drive"), 1f, 8f,
                    SpatializerBands.DefaultSaturationDrive, BandColors[band], 23f);
                DrawKnob(wets[band], new GUIContent("Wet", "Saturation Wet mix"), 0f, 1f,
                    SpatializerBands.DefaultSaturationWet, BandColors[band], 23f);
            }
        }
    }

    private void DrawGraph(Rect panel)
    {
        Rect plot = Rect.MinMaxRect(panel.x + 34f, panel.y + 25f, panel.xMax - 21f, panel.yMax - 24f);
        if (axisLabel == null)
        {
            axisLabel = new GUIStyle(EditorStyles.miniLabel) { alignment = TextAnchor.MiddleCenter };
            axisLabel.normal.textColor = GraphText;
        }
        EditorGUI.DrawRect(panel, new Color32(23, 29, 38, 255));
        EditorGUI.DrawRect(plot, new Color32(30, 38, 49, 255));

        for (int i = 0; i < gains.Length; i++)
        {
            GetBandEdges(i, out float left, out float right);
            left = FrequencyX(plot, left);
            right = FrequencyX(plot, right);
            Color fill = BandColors[i];
            fill.a = 0.075f;
            EditorGUI.DrawRect(new Rect(left, plot.y, right - left, plot.height), fill);
        }
        Color grid = new Color(1f, 1f, 1f, 0.12f);
        for (int i = 0; i < FrequencyTicks.Length; i++)
        {
            float x = FrequencyX(plot, FrequencyTicks[i]);
            EditorGUI.DrawRect(new Rect(x, plot.y, 1f, plot.height), grid);
            if (plot.width >= 260f || i % 2 == 0 || i == FrequencyTicks.Length - 1)
                GUI.Label(new Rect(x - 19f, plot.yMax + 3f, 38f, 16f), FrequencyLabels[i], axisLabel);
        }
        foreach (float db in GainTicks)
        {
            float y = DbY(plot, db);
            EditorGUI.DrawRect(new Rect(plot.x, y, plot.width, 1f),
                db == 0f ? new Color(1f, 1f, 1f, 0.32f) : grid);
            GUI.Label(new Rect(panel.x, y - 8f, 31f, 16f), db.ToString("+0;-0;0"), axisLabel);
        }
        GUI.Label(new Rect(panel.x, panel.y + 2f, 31f, 16f), "dB", axisLabel);

        if (Event.current.type == EventType.Repaint)
            DrawResponse(plot);

        for (int i = 0; i < crossovers.Length; i++)
        {
            SerializedProperty property = crossovers[i];
            float x = FrequencyX(plot, property.floatValue);
            Vector2 center = new Vector2(x, plot.y - 10f);
            EditorGUI.DrawRect(new Rect(x, plot.y, 1f, plot.height), new Color(0.7f, 0.77f, 0.85f, 0.48f));
            if (Event.current.type == EventType.Repaint)
            {
                using (new Handles.DrawingScope(CrossoverColor))
                {
                    Handles.DrawAAConvexPolygon(center + Vector2.up * 6f, center + Vector2.right * 6f,
                        center + Vector2.down * 6f, center + Vector2.left * 6f);
                }
            }
            Rect hit = CenteredRect(center, 18f);
            using (new EditorGUI.PropertyScope(hit, GUIContent.none, property))
            {
                if (Drag(hit, MouseCursor.ResizeHorizontal, property, out Vector2 delta))
                {
                    GetCrossoverLimits(i, out float min, out float max);
                    property.floatValue = delta.x == 0f ? dragValue
                        : Mathf.Clamp(Mathf.Round(XFrequency(plot, FrequencyX(plot, dragValue) + delta.x)), min, max);
                }
            }
        }

        for (int i = 0; i < gains.Length; i++)
        {
            GetBandEdges(i, out float left, out float right);
            SerializedProperty property = gains[i];
            Vector2 center = new Vector2((FrequencyX(plot, left) + FrequencyX(plot, right)) * 0.5f,
                DbY(plot, GainDb(property.floatValue)));
            DrawDisc(center, 9f, new Color32(14, 20, 28, 255));
            DrawDisc(center, 6.5f, BandColors[i]);
            Rect hit = CenteredRect(center, 20f);
            using (new EditorGUI.PropertyScope(hit, GUIContent.none, property))
            {
                if (Drag(hit, MouseCursor.ResizeVertical, property, out Vector2 delta))
                {
                    float y = DbY(plot, GainDb(dragValue)) + delta.y;
                    float t = Mathf.InverseLerp(plot.yMax, plot.yMin, y);
                    if (delta.y == 0f)
                        property.floatValue = dragValue;
                    else
                        property.floatValue = t <= 0f ? 0f : Mathf.Pow(10f, Mathf.Lerp(MinDb, MaxDb, t) / 20f);
                }
            }
        }
    }

    private void DrawResponse(Rect plot)
    {
        int sampleRate = AudioSettings.outputSampleRate;
        if (sampleRate < 1000)
            sampleRate = 48000;
        float ceiling = Mathf.Clamp(sampleRate * 0.45f, 80f, MaxHz);
        float low = Mathf.Clamp(crossovers[0].floatValue, MinHz, ceiling - 2f);
        float mid = Mathf.Clamp(crossovers[1].floatValue, low + 1f, ceiling - 1f);
        float high = Mathf.Clamp(crossovers[2].floatValue, mid + 1f, ceiling);
        float warpedLow = Mathf.Tan(Mathf.PI * low / sampleRate);
        float warpedMid = Mathf.Tan(Mathf.PI * mid / sampleRate);
        float warpedHigh = Mathf.Tan(Mathf.PI * high / sampleRate);
        float lowGain = gains[0].floatValue;
        float lowMidGain = gains[1].floatValue;
        float highMidGain = gains[2].floatValue;
        float highGain = gains[3].floatValue;
        float end = FrequencyX(plot, Mathf.Min(MaxHz, sampleRate * 0.5f));
        for (int i = 0; i < curve.Length; i++)
        {
            float x = Mathf.Lerp(plot.x, end, i / (float)(curve.Length - 1));
            float frequency = XFrequency(plot, x);
            float warped = Mathf.Tan(Mathf.PI * frequency / sampleRate);
            float lowWeight = LowPassWeight(warped / warpedLow);
            float midWeight = LowPassWeight(warped / warpedMid);
            float highWeight = LowPassWeight(warped / warpedHigh);
            float lower = Mathf.Lerp(lowMidGain, lowGain, lowWeight);
            float upper = Mathf.Lerp(highGain, highMidGain, highWeight);
            curve[i] = new Vector3(x, DbY(plot, GainDb(Mathf.Lerp(upper, lower, midWeight))));
        }
        using (new Handles.DrawingScope(enabledBands.boolValue ? Color.white : GraphText))
            Handles.DrawAAPolyLine(2f, curve);
    }

    // Phase compensation gives both branches the same phase, so their LR4 magnitudes can be weighted and summed.
    private static float LowPassWeight(float ratio)
    {
        float squared = ratio * ratio;
        return 1f / (1f + squared * squared);
    }

    private void DrawKnob(SerializedProperty property, GUIContent label, float min, float max, float defaultValue,
        Color color, float diameter, bool logarithmic = false)
    {
        using (new EditorGUILayout.VerticalScope(GUILayout.ExpandWidth(true)))
        {
            Rect row = GUILayoutUtility.GetRect(36f, diameter + 5f, GUILayout.ExpandWidth(true));
            Rect knob = CenteredRect(row.center, diameter);
            using (var scope = new EditorGUI.PropertyScope(knob, label, property))
            {
                float value = property.floatValue;
                float t = logarithmic
                    ? Mathf.InverseLerp(Mathf.Log(min), Mathf.Log(max), Mathf.Log(value))
                    : Mathf.InverseLerp(min, max, value);
                float angle = Mathf.Lerp(-135f, 135f, t) * Mathf.Deg2Rad;
                Vector2 direction = new Vector2(Mathf.Sin(angle), -Mathf.Cos(angle));
                DrawDisc(knob.center, diameter * 0.5f, new Color32(35, 43, 55, 255));
                DrawDisc(knob.center, diameter * 0.5f - 2f, new Color32(60, 72, 87, 255));
                DrawDisc(knob.center + direction * (diameter * 0.5f - 5f), 2.5f, color);
                if (Drag(knob, MouseCursor.ResizeVertical, property, out Vector2 delta,
                    resetValue: Mathf.Clamp(defaultValue, min, max)))
                {
                    float shift = (delta.x - delta.y) / 120f;
                    float next = logarithmic
                        ? Mathf.Round(dragValue * Mathf.Pow(max / min, shift))
                        : dragValue + shift * (max - min);
                    property.floatValue = shift == 0f ? dragValue : Mathf.Clamp(next, min, max);
                }
                Rect caption = GUILayoutUtility.GetRect(0f, 14f, GUILayout.ExpandWidth(true));
                GUI.Label(caption, scope.content, EditorStyles.centeredGreyMiniLabel);
                Rect field = GUILayoutUtility.GetRect(0f, EditorGUIUtility.singleLineHeight, GUILayout.ExpandWidth(true));
                EditorGUI.BeginChangeCheck();
                float entered = EditorGUI.DelayedFloatField(field, property.floatValue);
                if (EditorGUI.EndChangeCheck() && !float.IsNaN(entered) && !float.IsInfinity(entered))
                    property.floatValue = Mathf.Clamp(entered, min, max);
            }
        }
    }

    private bool Drag(Rect rect, MouseCursor cursor, SerializedProperty property, out Vector2 delta,
        float? resetValue = null)
    {
        int id = GUIUtility.GetControlID(FocusType.Passive);
        Event current = Event.current;
        EditorGUIUtility.AddCursorRect(rect, cursor, id);
        delta = Vector2.zero;
        switch (current.GetTypeForControl(id))
        {
            case EventType.MouseDown when current.button == 0 && rect.Contains(current.mousePosition) && GUI.enabled:
                GUI.FocusControl(null);
                if (current.clickCount == 2 && resetValue.HasValue)
                {
                    Undo.SetCurrentGroupName("Reset Spatializer " + property.displayName);
                    property.floatValue = resetValue.Value;
                    current.Use();
                    Repaint();
                    break;
                }
                GUIUtility.hotControl = id;
                dragOrigin = current.mousePosition;
                dragValue = property.floatValue;
                Undo.IncrementCurrentGroup();
                undoGroup = Undo.GetCurrentGroup();
                Undo.SetCurrentGroupName("Adjust Spatializer " + property.displayName);
                current.Use();
                break;
            case EventType.MouseDrag when GUIUtility.hotControl == id:
                delta = current.mousePosition - dragOrigin;
                current.Use();
                Repaint();
                return true;
            case EventType.MouseUp when current.button == 0 && GUIUtility.hotControl == id:
                GUIUtility.hotControl = 0;
                Undo.CollapseUndoOperations(undoGroup);
                current.Use();
                break;
        }
        return false;
    }

    private void GetCrossoverLimits(int index, out float min, out float max)
    {
        min = index == 0 ? MinHz : crossovers[index - 1].floatValue + 1f;
        max = index == crossovers.Length - 1 ? MaxHz : crossovers[index + 1].floatValue - 1f;
    }

    private void GetBandEdges(int band, out float left, out float right)
    {
        left = band == 0 ? MinHz : crossovers[band - 1].floatValue;
        right = band == crossovers.Length ? MaxHz : crossovers[band].floatValue;
    }

    private static Rect CenteredRect(Vector2 center, float size) =>
        new Rect(center.x - size * 0.5f, center.y - size * 0.5f, size, size);

    private static float FrequencyX(Rect plot, float hz) =>
        Mathf.Lerp(plot.xMin, plot.xMax, Mathf.Log10(hz / MinHz) / 3f);

    private static float XFrequency(Rect plot, float x) =>
        MinHz * Mathf.Pow(10f, 3f * Mathf.InverseLerp(plot.xMin, plot.xMax, x));

    private static float GainDb(float gain) => gain > 0f ? 20f * Mathf.Log10(gain) : MinDb;

    private static float DbY(Rect plot, float db) =>
        Mathf.Lerp(plot.yMax, plot.yMin, Mathf.InverseLerp(MinDb, MaxDb, db));

    private static void DrawDisc(Vector2 center, float radius, Color color)
    {
        if (Event.current.type != EventType.Repaint)
            return;
        using (new Handles.DrawingScope(color))
            Handles.DrawSolidDisc(center, Vector3.forward, radius);
    }
}

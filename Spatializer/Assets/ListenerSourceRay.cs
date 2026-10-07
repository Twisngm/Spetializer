using UnityEngine;

public sealed class ListenerSourceRay : MonoBehaviour
{
    [SerializeField] private Transform listener;
    [SerializeField] private Transform testSource;

    private void OnDrawGizmos()
    {
        if (listener == null || testSource == null)
            return;

        Gizmos.color = Color.yellow;
        Gizmos.DrawRay(listener.position, testSource.position - listener.position);
    }
}

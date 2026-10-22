"""Conservative live decisions for checkpoints without a qualified presence head.

Candidate scores and the structured-RF proxy never establish drone presence.
No deployed presence/identity policy exists yet; do not infer one from softmax.
"""


def apply_unqualified_policy(result):
    result = dict(result)
    result.update(
        presence_status=('insufficient_evidence' if result.get('status') == 'insufficient_evidence'
                         else 'unclassified_rf'),
        drone_present=None,
        drone_presence_probability=None,
        accepted_class=None,
        presence_validated=False,
        identity_validated=False,
        presence_reason='no_qualified_background_or_drone_presence_detector',
    )
    if result.get('status') == 'known_class_candidate':
        result.update(status='identification_unavailable', reason='live_acceptance_not_qualified')
    return result

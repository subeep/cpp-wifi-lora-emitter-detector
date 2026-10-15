"""Group-balanced development tradeoffs; never consumes final test labels/scores."""
import numpy as np


def group_mean(values, groups, mask):
    groups = np.asarray(groups)
    return float(np.mean([np.mean(values[mask & (groups == g)]) for g in sorted(set(groups[mask]))]))


def balanced_outcomes(prediction, labels, groups):
    labels = np.asarray(labels)
    known = labels >= 0
    return dict(correct_accepted=group_mean(prediction == labels, groups, known),
                wrong_accepted=group_mean((prediction >= 0) & (prediction != labels), groups, known),
                known_rejected=group_mean(prediction < 0, groups, known),
                unknown_accepted=group_mean(prediction >= 0, groups, ~known))


def choose(probabilities, distances, labels, groups, eligible, classes, accept):
    labels, eligible = np.asarray(labels), np.asarray(eligible, dtype=bool)
    abstention = dict(distance_quantile=None, min_probability=None, min_margin=None,
                     correct_accepted=0., wrong_accepted=0., known_rejected=1., unknown_accepted=0.,
                     objective=0., decision='abstain_no_supported_operating_point')
    best, trials = ((0., 0., 0., 0.), None, abstention), [abstention]
    predicted = probabilities.argmax(1)
    limits = []
    for k in range(classes):
        correct = (labels == k) & (predicted == k) & eligible
        if correct.sum() < 5 or len(set(np.asarray(groups)[correct])) < 2:
            raise ValueError('joint calibration requires correct eligible examples from two groups per class')
        limits.append(distances[correct, k])
    for quantile in (.9, .95, 1.0):
        for minimum in (.4, .6, .8, .9, .95):
            for margin in (0.0, .1, .2):
                policy = [dict(min_probability=minimum, min_margin=margin,
                               max_distance=float(np.quantile(limits[k], quantile))) for k in range(classes)]
                decisions = accept(probabilities, distances, policy)
                decisions[~eligible] = -1
                result = balanced_outcomes(decisions, labels, groups)
                objective = result['correct_accepted'] - 2 * result['wrong_accepted'] - 2 * result['unknown_accepted']
                trial = dict(distance_quantile=quantile, min_probability=minimum, min_margin=margin,
                             **result, objective=objective)
                trials.append(trial)
                key = (objective, result['correct_accepted'], -result['wrong_accepted'], -result['unknown_accepted'])
                if key > best[0]:
                    best = key, policy, trial
    return best[1], dict(selected=best[2], trials=trials,
                        enabled=best[1] is not None,
                        scope='group_balanced_known_and_unknown_calibration_only',
                        objective='correct_accept - 2*wrong_accept - 2*unknown_accept',
                        operational_guarantee=False)

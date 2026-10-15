"""Compact spectrogram CNN; no pretrained image or metadata inputs."""
import torch
from torch import nn
from torch.nn import functional as F


class SpectrogramCNN(nn.Module):
    def __init__(self, classes):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 16, 5, stride=2, padding=2), nn.GroupNorm(4, 16), nn.SiLU(),
            nn.Conv2d(16, 32, 3, stride=2, padding=1), nn.GroupNorm(8, 32), nn.SiLU(),
            nn.Conv2d(32, 64, 3, stride=2, padding=1), nn.GroupNorm(8, 64), nn.SiLU(),
            nn.Conv2d(64, 96, 3, stride=2, padding=1), nn.GroupNorm(8, 96), nn.SiLU(),
            nn.AdaptiveAvgPool2d((4, 4)), nn.Flatten(), nn.Linear(96 * 16, 64), nn.SiLU())
        self.classifier = nn.Linear(64, classes)

    def forward(self, x):
        embedding = self.features(x)
        return self.classifier(embedding), F.normalize(embedding, dim=1)
